/*
 * clock.c
 *
 * Clocks of the ADC/DMA example on the dsPIC33AK512MPS512:
 *   FRC 8 MHz -> PLL1 -> CLKGEN6 = 320 MHz ADC input clock
 *             -> PLL2 -> CLKGEN1 = 200 MHz system clock
 * plus the fail-safe clock monitor's interrupt.
 *
 *   TAD = 4 / 320 MHz = 12.5 ns, throughput 40 MSPS (DS70005591D, AD50/AD51).
 *   CLKGEN6 is the ADC clock source per DS70005591D Table 16-1.
 *
 * Nothing here knows the ADC or the DMA; every register value is
 * commented with the datasheet page it comes from.
 */

#include <xc.h>
#include "clock.h"
#include "capture.h"
#include "console.h"
#include "diag.h"

/* NOSC / COSC values, from the ATDF value-group CLK1_CON__COSC. */
#define NOSC_FRC        0x1u
#define NOSC_PLL1_OUT   0x5u
#define NOSC_PLL2_OUT   0x6u

bool clock_cpu_on_pll(void)
{
    return CLK1CONbits.COSC == NOSC_PLL2_OUT;
}

uint32_t clock_cpu_hz(void)
{
    return clock_cpu_on_pll() ? 200000000ul : 8000000ul;
}

/* ------------------------------------------------------------------ *
 * Clock setup
 *
 * Two PLLs, because the two rates this example needs are both exact
 * multiples of the 8 MHz FRC and neither needs a fractional divider:
 *
 *   PLL1 -> 320 MHz -> CLKGEN6 -> ADC   (TAD = 4/320 MHz = 12.5 ns)
 *   PLL2 -> 200 MHz -> CLKGEN1 -> CPU, DMA and the standard peripherals
 *
 * THE SWITCHING ORDER IS NOT OPTIONAL. DS70005591D page 778 spells it
 * out, and skipping a step does not fail loudly - it leaves the old
 * divider values in place and the part runs at the wrong speed:
 *
 *   a) set PLLSWEN  -> allows the input and feedback dividers to update
 *   c) set FOUTSWEN -> allows the output dividers to update
 *   d) select the source in NOSC
 *   e) set OSWEN    -> perform the switch
 *
 * Each of those bits clears itself when its step has completed, so each
 * one is followed by a (bounded) wait.
 *
 * Also from page 778: "The output dividers POSTDIV1 and POSTDIV2 should
 * not be changed while the PLL is operating", and POSTDIV1 must be >=
 * POSTDIV2.
 *
 * The divider values below are the ones Microchip's own MCC-generated
 * example uses for this part on this board, which is the reason to prefer
 * them over an equally valid arithmetic alternative - they have run on
 * hardware:
 *   https://github.com/microchip-pic-avr-examples/dspic33ak-curiosity-adc-40msps
 *
 *   PLL1DIV = 0x0100C829 : N1=1, M=200, POSTDIV1=5, POSTDIV2=1
 *                          8 MHz -> FVCO 1600 MHz -> 320 MHz
 *   PLL2DIV = 0x01007D29 : N1=1, M=125, POSTDIV1=5, POSTDIV2=1
 *                          8 MHz -> FVCO 1000 MHz -> 200 MHz
 *
 * Bit layout of PLLxDIV (ATDF): POSTDIV2[2:0], POSTDIV1[5:3],
 * PLLFBDIV[16:8], PLLPRE[27:24].
 *
 * Constraints checked against Table 40-23 and page 777: F_PFD >= 5 MHz,
 * F_VCO 500...1600 MHz, M in 16...320, POSTDIV1 >= POSTDIV2.
 *
 * OSCCTRL.PLLxEN is deliberately NOT written here. It looks like it should
 * be - 12.4.6 (p776) says "the PLLs can be enabled by PLLxEN bits" - but
 * neither the normative procedure (12.4.6.3, Example 12-4, p781) nor the
 * MCC example ever sets it; both rely on PLLxCON.ON, whose clock-request
 * path enables the PLL. A first attempt on hardware additionally set
 * PLLxEN and waited for PLLxRDY *before* the first divider write, on the
 * strength of Example 16-3 (p1328) - but that snippet sits in the ADC
 * chapter's gain-calibration example, is duplicated verbatim in Example
 * 17-4 (p1392) with a contradictory comment, and its own arithmetic does
 * not add up (FBDIV=80, POSTDIV1=4 is 160 MHz, not the 320 MHz claimed).
 * Waiting for PLLxRDY before the dividers means waiting for a lock on the
 * POR configuration (M=200, POSTDIV1=2, POSTDIV2=2 -> 400 MHz), which is
 * exactly the kind of intermediate state 12.4.6.1 note 2 (p779) warns
 * about. Do not reintroduce it; follow the MCC order, which has run on
 * silicon.
 * ------------------------------------------------------------------ */
void clock_init(void)
{
    /* If the system clock is currently running off a PLL, park it on the
     * FRC first. Changing PLL settings underneath a running CPU clock can
     * overclock the core - this matters on a debugger restart, where the
     * part is not freshly reset. (The MCC example does the same.) */
    console_trace_kv_hex("[clk] CLK1CON at entry", CLK1CON);
    if ((CLK1CONbits.COSC >= NOSC_PLL1_OUT) && (CLK1CONbits.COSC <= 0x8u)) {
        console_trace("[clk] system clock on a PLL, parking on FRC\r\n");
        CLK1CONbits.NOSC  = NOSC_FRC;
        CLK1CONbits.OSWEN = 1u;
        WAIT_WHILE(CLK1CONbits.OSWEN, 3u);
    }

    /* ---- PLL1: 320 MHz for the ADC ---- */
    PLL1CON = 0x8100u;          /* ON = 1, NOSC = FRC                   */
    PLL1DIV = 0x0100C829u;      /* N1=1, M=200, POSTDIV1=5, POSTDIV2=1  */

    PLL1CONbits.PLLSWEN  = 1u;  /* (a) apply input and feedback dividers */
    WAIT_WHILE(PLL1CONbits.PLLSWEN, 1u);
    PLL1CONbits.FOUTSWEN = 1u;  /* (c) apply output dividers             */
    WAIT_WHILE(PLL1CONbits.FOUTSWEN, 1u);
    PLL1CONbits.OSWEN    = 1u;  /* (e) switch                            */
    WAIT_WHILE(PLL1CONbits.OSWEN, 1u);
    WAIT_WHILE(!OSCCTRLbits.PLL1RDY, 1u);

    VCO1DIV = 0x10000u;         /* VCO divider output, unused here       */
    PLL1CONbits.DIVSWEN = 1u;
    WAIT_WHILE(PLL1CONbits.DIVSWEN, 1u);
    console_trace("[clk] PLL1 locked, 320 MHz\r\n");

    /* ---- PLL2: 200 MHz for the system clock ---- */
    PLL2CON = 0x8100u;
    PLL2DIV = 0x01007D29u;      /* N1=1, M=125, POSTDIV1=5, POSTDIV2=1  */

    PLL2CONbits.PLLSWEN  = 1u;
    WAIT_WHILE(PLL2CONbits.PLLSWEN, 2u);
    PLL2CONbits.FOUTSWEN = 1u;
    WAIT_WHILE(PLL2CONbits.FOUTSWEN, 2u);
    PLL2CONbits.OSWEN    = 1u;
    WAIT_WHILE(PLL2CONbits.OSWEN, 2u);
    WAIT_WHILE(!OSCCTRLbits.PLL2RDY, 2u);

    VCO2DIV = 0x10000u;
    PLL2CONbits.DIVSWEN = 1u;
    WAIT_WHILE(PLL2CONbits.DIVSWEN, 2u);
    console_trace("[clk] PLL2 locked, 200 MHz\r\n");
    /* Let that line leave the shift register before the CPU clock, and
     * with it the baud rate, changes 25x. Seen on the board: without
     * this the tail of the line came out as garbage. */
    console_flush();

    /* ---- CLKGEN1 = system clock, from PLL2, no divider ----
     * DS70005591D 12.4.9, p795: "Clock Generator 1 is the clock source
     * for the system clock (sys_clk) and peripheral clock." */
    CLK1CON = 0x129600u;        /* NOSC = PLL2 out, ON, backup BFRC, FSCM */
    CLK1DIV = 0u;               /* 200 MHz straight through               */
    CLK1CONbits.OSWEN = 1u;
    WAIT_WHILE(CLK1CONbits.OSWEN, 3u);
    /* From here on the CPU runs at 200 MHz and the UART's baud generator
     * is off by 25x until cli_init() re-sets it - so no trace output
     * until then. */

    /* ---- CLKGEN6 = ADC clock, from PLL1, no divider ----
     * DS70005591D Table 16-1, p1223 names CLKGEN6 as the ADC clock
     * source, range 32...320 MHz. 320 MHz is the maximum (Table 40-24,
     * p2016) and gives TAD = 12.5 ns, hence 40 MSPS (AD50/AD51). */
    CLK6CON = 0x29500u;         /* NOSC = PLL1 out, ON                   */
    CLK6DIV = 0u;               /* 320 MHz straight through              */
    CLK6CONbits.OSWEN = 1u;
    WAIT_WHILE(CLK6CONbits.OSWEN, 4u);

    /* The fail-safe clock monitor is on (FSCMEN in the CLK1CON value
     * above). When it sees the system clock stop it moves the CPU to the
     * backup FRC and raises IRQ 9 (CLKFAIL). Left masked, that only sets
     * a flag: the board would carry on at 8 MHz with a garbled console and
     * a wrong sample rate, and nothing would say why. Enabled, it lands
     * in _CLKFInterrupt(), which reports the event and stops in fail(10). */
    IFS0bits.CLKFAILIF = 0u;
    IEC0bits.CLKFAILIE = 1u;
}

/* IRQ 9, IVT slot 17: the fail-safe clock monitor moved the CPU off the
 * PLL. The name follows the pack's interrupt list ("CLKFInterrupt");
 * that the linker put it into slot 17 was checked on the built ELF.
 * The console is re-initialised from scratch because the CPU is now on
 * the 8 MHz BFRC and the baud divider was set for 100 MHz. */
void __attribute__((interrupt, no_auto_psv)) _CLKFInterrupt(void)
{
    IFS0bits.CLKFAILIF = 0u;
    capture_halt();
    console_force_up();
    console_puts("\r\n[CLKF] clock fail: the FSCM switched the CPU to the backup FRC\r\n");
    console_kv_hex("[CLKF] OSCCTRL", OSCCTRL);
    console_kv_hex("[CLKF] PLL2CON", PLL2CON);
    console_kv_hex("[CLKF] CLK1CON", CLK1CON);
    console_kv("[CLKF] reached boot stage", boot_stage);
    fail(10u);
}

void clock_regs_dump(void)
{
    console_puts("[regs] clock\r\n");
    console_kv_hex("OSCCTRL", OSCCTRL);
    console_kv_hex("PLL1CON", PLL1CON);
    console_kv_hex("PLL1DIV", PLL1DIV);
    console_kv_hex("PLL2CON", PLL2CON);
    console_kv_hex("PLL2DIV", PLL2DIV);
    console_kv_hex("CLK1CON", CLK1CON);
    console_kv_hex("CLK1DIV", CLK1DIV);
    console_kv_hex("CLK6CON", CLK6CON);
    console_kv_hex("CLK6DIV", CLK6DIV);
    console_kv_hex("IEC0", IEC0);           /* CLKFAIL enable, bit 9     */
    console_kv_hex("IFS0", IFS0);           /* CLKFAIL flag,   bit 9     */
}

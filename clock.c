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

/* ------------------------------------------------------------------ *
 * ADC clock divider at run time (clock.h). INTDIV is bits 30:16 of
 * CLK6DIV. The generator is taken down and brought up again around the
 * write (ON, then DIVSWEN until the hardware clears it, then CLKRDY) -
 * the boot sequence repeated, as datasheet Example 12-2 orders it.
 * Nothing else in the tree changes: the CPU stays on PLL2, the
 * peripherals on their own generators. The ADC is off meanwhile
 * (capture.c): its clock is set before it is enabled, as at boot, not
 * changed under a running core.
 * ------------------------------------------------------------------ */
#define ADC_CLK_HZ        320000000u
#define DIVSW_WAIT_LIMIT  100000u     /* loop iterations, far above the switch */

uint32_t clock_adc_set_div(uint32_t ratio_h)
{
    if ((ratio_h < 100u) || (ratio_h > 1000u)) {
        return CLKDIV_RANGE;
    }
    /* "FRACDIV will not work if INTDIV is configured to 0" - DS70005591D
     * 12.4.2 step 4b, p771. INTDIV is ratio/2, so any ratio between 1 and
     * 2 would need INTDIV 0 plus a fraction, which the hardware ignores:
     * the clock would come out undivided and the log would show a rate
     * that has nothing to do with the ratio asked for. Refused instead.
     * Ratio 1 IS the undivided clock and is written as both fields 0. */
    if ((ratio_h > 100u) && (ratio_h < 200u)) {
        return CLKDIV_INTDIV0;
    }
    /* Fdiv = Fin / (2 * (INTDIV + FRACDIV/512)) (Example 12-2, p771), so
     * with the ratio in hundredths INTDIV is the integer part of
     * ratio_h/200 and FRACDIV the rest scaled to 512, half rounded.
     * Worked examples: 1000 -> 5/0, 900 -> 4/256, 500 -> 2/256,
     * 450 -> 2/128, 250 -> 1/128, 200 -> 1/0, 100 -> 0/0. */
    const uint32_t intdiv  = (ratio_h == 100u) ? 0u : (ratio_h / 200u);
    const uint32_t fracdiv = (ratio_h == 100u) ? 0u
                             : (((ratio_h % 200u) * 512u + 100u) / 200u);
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    (void)intdiv; (void)fracdiv;       /* no clock tree to switch          */
    return CLKDIV_OK;
#else
    /* THE GENERATOR STAYS ON while the divider changes.
     *
     * Until run 8 (24.09.2026) this switched CLKGEN6 off, wrote the
     * divider, switched it back on and only then asked for the update.
     * Every step reported success - all fifteen ratios were written and
     * read back correctly, DIVSWEN cleared, CLKRDY came - and the ADC
     * kept converting at 40 MSPS at every single one of them. The divide
     * factor was never taken over.
     *
     * The documented procedure (12.4.2 step 4 and Example 12-2, p771)
     * never turns the generator off: with it running, write INTDIV, then
     * FRACDIV, then set DIVSWEN and wait for the hardware to clear it.
     * Do not reintroduce the off/on. The ADC core and the DMA channel are
     * taken down by the caller, which is what needs protecting; the clock
     * generator does not.
     *
     * Every step is still checked, because run 7 could not tell a divider
     * that never switched from one that switched without changing the
     * rate: the fields are read back before the switch and again after
     * it, and both waits report themselves. Note what that check can and
     * cannot say - it proves the register holds the value, not that the
     * clock changed. Only a measured rate proves that. */
    CLK6DIVbits.INTDIV  = intdiv;      /* integer factor first (12.4.2 4a) */
    CLK6DIVbits.FRACDIV = fracdiv;     /* then the fraction    (12.4.2 4b) */
    if ((CLK6DIVbits.INTDIV != intdiv) || (CLK6DIVbits.FRACDIV != fracdiv)) {
        return CLKDIV_NOT_WRITTEN;     /* the write did not reach CLK6DIV  */
    }
    CLK6CONbits.DIVSWEN = 1u;          /* apply them           (12.4.2 4c) */
    uint32_t n = DIVSW_WAIT_LIMIT;
    while (CLK6CONbits.DIVSWEN && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_DIVSWEN; }   /* switch never completed   */
    n = DIVSW_WAIT_LIMIT;
    while (!CLK6CONbits.CLKRDY && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_CLKRDY; }    /* generator not ready      */
    if ((CLK6DIVbits.INTDIV != intdiv) || (CLK6DIVbits.FRACDIV != fracdiv)) {
        return CLKDIV_LOST;            /* the switch discarded the value   */
    }
    return CLKDIV_OK;
#endif
}

const char *clock_adc_div_error(uint32_t rc)
{
    switch (rc) {
    case CLKDIV_OK:          return "the configuration arrived";
    case CLKDIV_RANGE:       return "ratio outside 100..1000";
    case CLKDIV_NOT_WRITTEN: return "CLK6DIV did not take the value";
    case CLKDIV_DIVSWEN:     return "DIVSWEN never cleared - no divider switch";
    case CLKDIV_CLKRDY:      return "CLKRDY never came - generator not running";
    case CLKDIV_LOST:        return "CLK6DIV lost the value over the switch";
    case CLKDIV_ADC:         return "ADC core did not report ADRDY after the switch";
    case CLKDIV_INTDIV0:     return "ratio below 2 needs INTDIV 0, where FRACDIV does not work";
    default:                 return "unknown";
    }
}

void clock_adc_off(void)
{
    CLK6CONbits.ON = 0u;
}

bool clock_adc_on(void)
{
    CLK6CONbits.ON = 1u;
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return true;
#else
    uint32_t n = DIVSW_WAIT_LIMIT;
    while (!CLK6CONbits.CLKRDY && (--n != 0u)) { }
    return n != 0u;
#endif
}

/* ------------------------------------------------------------------ *
 * CLKGEN7 for the DAC (clock.h). Same recipe as CLKGEN6 at boot: source
 * PLL1 Fout, no divider, switch, wait. Table 18-1 (p1385) names Clock
 * Generator 7 as the DAC clock.
 * ------------------------------------------------------------------ */
bool clock_dac_on(void)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return true;
#else
    CLK7CON = 0x29500u;             /* NOSC = PLL1 out, ON, backup BFRC  */
    CLK7DIV = 0u;
    CLK7CONbits.OSWEN = 1u;
    uint32_t n = DIVSW_WAIT_LIMIT;
    while (CLK7CONbits.OSWEN && (--n != 0u)) { }
    if (n == 0u) { return false; }
    n = DIVSW_WAIT_LIMIT;
    while (!CLK7CONbits.CLKRDY && (--n != 0u)) { }
    return n != 0u;
#endif
}

void clock_dac_off(void)
{
    CLK7CONbits.ON = 0u;
}

uint32_t clock_dac_hz(void)
{
    return ADC_CLK_HZ;              /* PLL1 Fout, undivided              */
}

uint32_t clock_adc_div(void)
{
    /* Back from the register, in hundredths: 2 * (INTDIV + FRACDIV/512)
     * * 100 = (INTDIV * 512 + FRACDIV) * 200 / 512. Both fields 0 =
     * straight through = 100. */
    const uint32_t raw = CLK6DIVbits.INTDIV * 512u + CLK6DIVbits.FRACDIV;
    return (raw == 0u) ? 100u : (raw * 200u + 256u) / 512u;
}

uint32_t clock_adc_hz(void)
{
    return (uint32_t)(((uint64_t)ADC_CLK_HZ * 100u) / clock_adc_div());
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
    console_kv_hex("CLK7CON", CLK7CON);     /* DAC clock                 */
    console_kv_hex("IEC0", IEC0);           /* CLKFAIL enable, bit 9     */
    console_kv_hex("IFS0", IFS0);           /* CLKFAIL flag,   bit 9     */
}

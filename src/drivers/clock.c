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
#include "timebase.h"

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

    /* VCO divider output = the DAC clock (CLKGEN7, NOSC 7). INTDIV[30:16],
     * F = FVCO / (2 * INTDIV) (12.3.9; Example 12-4, p781, writes the same
     * INTDIV = 2 with "PLL VCO DIV = PLL VCO clock / 2* INTDIV"): 1600 MHz
     * / 4 = 400 MHz, the DAC's minimum (Table 40-24, p2016: 400-500 MHz).
     * It used to be 0x10000 = 800 MHz and unused, with the DAC on PLL1 out
     * at 320 MHz - below its minimum (ANALYSIS.md C.12.1). */
    VCO1DIV = 0x20000u;
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

    /* PLL2 VCO divider: 1000 MHz / (2 * 1) = 500 MHz. Nothing runs on it
     * except, on request, the DAC as a cross-check on a second VCO
     * (clock_dac_select(), chaintest S5). 500 MHz is the DAC's maximum. */
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
#define ADC_CLK_HZ        320000000u   /* PLL1 output as clock_init() sets it */
#define FRC_HZ              8000000u   /* the internal FRC, PLL1 input        */
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
    case CLKDIV_FOUTSWEN:    return "FOUTSWEN never cleared - PLL1 output dividers not applied";
    case CLKDIV_PLLRDY:      return "PLL1 did not lock again";
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
 * CLKGEN7 for the DAC (clock.h). Table 18-1 (p1385) names Clock
 * Generator 7 as the DAC clock, and Table 40-24 (p2016) wants 400 to
 * 500 MHz at its input. Source: the PLL1 VCO divider, 400 MHz (NOSC 7,
 * ATDF value group CLK_CON__NOSC "PLL1 VCO Divider output") - the same
 * VCO as the ADC and the SCCP1 trigger, so all three stay in a fixed
 * ratio (ANALYSIS.md C.11). The PLL2 VCO divider (NOSC 8, 500 MHz) is
 * the second choice, for the cross-check on another VCO.
 *
 * Until 25.09.2026 this was CLK7CON = 0x29500, PLL1 out at 320 MHz:
 * below the DAC's minimum, for every DAC capture this project ever took.
 * The CON word is otherwise unchanged: ON, BOSC = BFRC, OE bit as before.
 * ------------------------------------------------------------------ */
#define CLK7CON_BASE    0x29000u    /* ON, backup BFRC, as 0x29500 had   */
static uint32_t dac_nosc = CLOCK_DAC_PLL1_VCO;

void clock_dac_select(uint32_t nosc)
{
    dac_nosc = nosc;
}

bool clock_dac_on(void)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return true;
#else
    CLK7CON = CLK7CON_BASE | (dac_nosc << 8);   /* NOSC[11:8]            */
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

/* PLL1's output as the registers say it is: FVCO = FRC * PLLFBDIV /
 * PLLPRE, output = FVCO / (POSTDIV1 * POSTDIV2). Both CLKGEN6 (ADC) and
 * CLKGEN7 (DAC) hang off it, so retuning the PLL for the sample rate
 * moves the DAC's clock with it - which is why dactest.c asks for the
 * period in nanoseconds rather than assuming one. */
static uint32_t pll1_out_hz(void)
{
    const uint32_t pre = PLL1DIVbits.PLLPRE   ? PLL1DIVbits.PLLPRE   : 1u;
    const uint32_t fb  = PLL1DIVbits.PLLFBDIV ? PLL1DIVbits.PLLFBDIV : 1u;
    const uint32_t p1  = PLL1DIVbits.POSTDIV1 ? PLL1DIVbits.POSTDIV1 : 1u;
    const uint32_t p2  = PLL1DIVbits.POSTDIV2 ? PLL1DIVbits.POSTDIV2 : 1u;
    const uint64_t vco = (uint64_t)FRC_HZ * fb / pre;
    return (uint32_t)(vco / ((uint64_t)p1 * p2));
}

bool clock_trig_on(void)
{
    /* Same source as CLKGEN6, so that the ADC and the module that
     * triggers it are fed from one clock - but divided by two: the CCP
     * modules may be clocked at 200 MHz at most (Table 40-24, p2016), and
     * PLL1 out is 320 MHz. INTDIV = 1 is a ratio of 2 (F = Fin / (2 *
     * INTDIV), Example 12-2, p771), 160 MHz, which is also what
     * Microchip's MC106 trigger example runs SCCP1 at. Until 25.09.2026
     * this had no divider and ran the module at 320 MHz, out of
     * specification. Whether the divider really divides is measured, not
     * assumed: chaintest S1 times CCP1TMR against Timer1.
     * Divider first, then the switch; DIVSWEN as well, because a CLKGEN
     * divider only takes a new value through DIVSWEN (12.4.2 step 4). */
    CLK13CON = 0x29500u;              /* NOSC = PLL1 out, ON            */
    CLK13DIV = 1ul << 16;             /* INTDIV = 1: /2 = 160 MHz       */
    CLK13CONbits.OSWEN = 1u;
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return true;
#else
    uint32_t n = DIVSW_WAIT_LIMIT;
    while (CLK13CONbits.OSWEN && (--n != 0u)) { }
    if (n == 0u) { return false; }
    CLK13CONbits.DIVSWEN = 1u;
    n = DIVSW_WAIT_LIMIT;
    while (CLK13CONbits.DIVSWEN && (--n != 0u)) { }
    if (n == 0u) { return false; }
    n = DIVSW_WAIT_LIMIT;
    while (!CLK13CONbits.CLKRDY && (--n != 0u)) { }
    return n != 0u;
#endif
}

void clock_trig_off(void)
{
    CLK13CONbits.ON = 0u;
}

uint32_t clock_trig_hz(void)
{
    /* Whatever PLL1 puts out, divided by CLK13DIV - derived, like the ADC
     * clock, so a generator that did not switch shows up in the number. */
    const uint32_t raw = CLK13DIVbits.INTDIV * 512u + CLK13DIVbits.FRACDIV;
    const uint32_t div = (raw == 0u) ? 100u : ((raw * 200u + 256u) / 512u);
    return (uint32_t)(((uint64_t)pll1_out_hz() * 100u) / div);
}

uint32_t clock_periph_hz(void)
{
    /* CLKGEN1 feeds the system clock and the peripheral clock; the
     * standard-speed peripheral clock is half the CPU clock (Timer1 at
     * 12.5 MHz with its 1:8 prescaler confirms it, and the timer check
     * reports 1 250 001 ticks per 100 ms against 1 250 000 expected). */
    return clock_cpu_hz() / 2u;
}

/* A PLL's VCO and its VCO divider output, from the registers. */
static uint32_t vco_hz(uint32_t pre, uint32_t fb)
{
    if (pre == 0u) { pre = 1u; }
    return (uint32_t)((uint64_t)FRC_HZ * fb / pre);
}

static uint32_t vcodiv_hz(uint32_t vco, uint32_t intdiv)
{
    return (intdiv == 0u) ? vco : (vco / (2u * intdiv));
}

uint32_t clock_dac_hz(void)
{
    /* Whatever CLKGEN7 is switched to, read back (CLK7DIV is straight
     * through). The triangle model in dac.c depends on this number. */
    switch (CLK7CONbits.COSC) {
    case CLOCK_DAC_PLL1_VCO:
        return vcodiv_hz(vco_hz(PLL1DIVbits.PLLPRE, PLL1DIVbits.PLLFBDIV), VCO1DIVbits.INTDIV);
    case CLOCK_DAC_PLL2_VCO:
        return vcodiv_hz(vco_hz(PLL2DIVbits.PLLPRE, PLL2DIVbits.PLLFBDIV), VCO2DIVbits.INTDIV);
    case NOSC_PLL1_OUT:
        return pll1_out_hz();
    case NOSC_FRC:
        return FRC_HZ;
    default:
        return 0u;
    }
}

/* ------------------------------------------------------------------ *
 * Clock monitor 4 as a frequency meter (clock.h)
 *
 * DS70005591D 12.4.7.3.1 and Example 12-5 (p788 f.): the reference clock
 * (WINSEL) opens a window of WINPR + 1 of its cycles, the monitored clock
 * (CNTSEL, divided by CNTDIV) is counted in it, and the count lands in
 * CMxBUF with BUFV set. Codes from the ATDF value group CM_SEL__CNTSEL,
 * the same list for WINSEL.
 *
 * Reference: CLKGEN1 (code 0), the 200 MHz system clock from PLL2 - the
 * clock Timer1 hangs off, whose calibration every run checks. Window
 * 200 000 cycles = 1 ms. Counter divided by 4 (CNTDIV = 2), so a 400 MHz
 * clock counts 100 000 per window and the counter never runs near its
 * limit; resolution 4 kHz. Thresholds wide open and the saturation at
 * the top, so the monitor only measures and never raises anything; its
 * interrupts stay disabled. CM4, because the fail-safe clock monitor of
 * CLKGEN1 (FSCMEN in CLK1CON) may be served by one of the others.
 *
 * The first window after ON can be a partial one, so the second capture
 * is the answer. No BUFV within the bound means the monitored clock does
 * not run (or the monitor does not work): 0.
 * ------------------------------------------------------------------ */
#define CM_WINDOW_CYCLES   200000u        /* of CLKGEN1 = 1 ms           */
#define CM_WINSEL_CLKGEN1  0u
#define CM_CNTDIV_4        2u

#ifndef __MPLAB_DEBUGGER_SIMULATOR
static bool cm4_wait_buffer(void)
{
    uint32_t n = DIVSW_WAIT_LIMIT * 10u;   /* ~ tens of ms at 200 MHz    */
    while (!CM4STATbits.BUFV && (--n != 0u)) { }
    return n != 0u;
}
#endif

uint32_t clock_monitor_hz(uint32_t cntsel)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    (void)cntsel;
    return 0u;
#else
    CM4CONbits.ON = 0u;
    CM4CON   = 0u;
    CM4SEL   = (cntsel << 8) | CM_WINSEL_CLKGEN1;   /* CNTSEL[15:8], WINSEL[7:0] */
    CM4WINPR = CM_WINDOW_CYCLES - 1u;               /* WINPR + 1 cycles (p789)   */
    CM4SAT   = 0xFFFFFFFFu;
    CM4HFAIL = 0xFFFFFFFFu;  CM4LFAIL = 0u;
    CM4HWARN = 0xFFFFFFFFu;  CM4LWARN = 0u;
    CM4CONbits.CNTDIV = CM_CNTDIV_4;
    CM4CONbits.ON = 1u;
    if (!cm4_wait_buffer()) { CM4CONbits.ON = 0u; return 0u; }
    /* The first capture may come from a partial window. Whether reading
     * BUF clears BUFV is not documented, so do not rely on the flag
     * again: wait 2.5 ms on Timer1 - at least one further complete 1 ms
     * window - and take what BUF holds then. */
    const uint32_t t0 = timebase_ticks();
    while ((timebase_ticks() - t0) < (TIMEBASE_HZ / 400u)) { }
    const uint32_t v = CM4BUF;
    CM4CONbits.ON = 0u;
    if (v == 0u) { return 0u; }
    /* count * CNTDIV (4) per 1 ms window */
    return v * 4u * (200000000u / CM_WINDOW_CYCLES);
#endif
}

uint32_t clock_adc_div(void)
{
    /* Back from the register, in hundredths: 2 * (INTDIV + FRACDIV/512)
     * * 100 = (INTDIV * 512 + FRACDIV) * 200 / 512. Both fields 0 =
     * straight through = 100. */
    const uint32_t raw = CLK6DIVbits.INTDIV * 512u + CLK6DIVbits.FRACDIV;
    return (raw == 0u) ? 100u : (raw * 200u + 256u) / 512u;
}

uint32_t clock_pll1_fbdiv(void)       { return PLL1DIVbits.PLLFBDIV; }

/* The rate this combination produces, in kSPS: PLLFBDIV / (p1*p2) MSPS,
 * because eight ADC clocks make one conversion and the ADC clock is
 * 8 MHz * PLLFBDIV / (p1*p2). */
static uint32_t rate_of(uint32_t fb, uint32_t p)
{
    return (p != 0u) ? ((fb * 1000u) / p) : 0u;
}

uint32_t clock_adc_set_rate(uint32_t want_ksps, uint32_t *got_ksps)
{
    /* The reachable range, straight from the two constraints: the VCO
     * needs PLLFBDIV 63..200, and the ADC wants 32...320 MHz in, which is
     * the same 4000...40000 kSPS out. */
    if ((want_ksps < 4000u) || (want_ksps > 40000u)) { return CLKDIV_RANGE; }

    uint32_t best_p1 = 5u, best_p2 = 1u, best_fb = 200u, best_k = 40000u;
    uint32_t best_d  = 0xFFFFFFFFu;
    for (uint32_t p1 = 1u; p1 <= 7u; p1++) {
        for (uint32_t p2 = 1u; p2 <= p1; p2++) {   /* POSTDIV1 >= POSTDIV2 */
            const uint32_t p  = p1 * p2;
            const uint32_t fb = ((want_ksps * p) + 500u) / 1000u;
            if ((fb < 63u) || (fb > 200u)) { continue; }
            const uint32_t k = rate_of(fb, p);
            if ((k < 4000u) || (k > 40000u)) { continue; }
            const uint32_t d = (k > want_ksps) ? (k - want_ksps) : (want_ksps - k);
            if (d < best_d) {
                best_d = d; best_p1 = p1; best_p2 = p2; best_fb = fb; best_k = k;
            }
        }
    }
    if (got_ksps != NULL) { *got_ksps = best_k; }
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    /* No PLL to retune here; the search above still runs, so that the
     * rate the caller is told is the one the board would pick. */
    (void)best_p1; (void)best_p2; (void)best_fb;
    return CLKDIV_OK;
#else
    /* One write, then the two update steps of clock_init(): PLLSWEN for
     * the input and feedback dividers, FOUTSWEN for the output dividers.
     * PLLPRE stays 1 - the 8 MHz input is already at the phase detector
     * minimum and dividing it further would only shrink the VCO range. */
    PLL1DIVbits.PLLFBDIV = best_fb;
    PLL1DIVbits.POSTDIV1 = best_p1;
    PLL1DIVbits.POSTDIV2 = best_p2;
    if ((PLL1DIVbits.PLLFBDIV != best_fb) ||
        (PLL1DIVbits.POSTDIV1 != best_p1) ||
        (PLL1DIVbits.POSTDIV2 != best_p2)) {
        return CLKDIV_NOT_WRITTEN;
    }
    PLL1CONbits.PLLSWEN = 1u;
    uint32_t n = DIVSW_WAIT_LIMIT;
    while (PLL1CONbits.PLLSWEN && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_FOUTSWEN; }
    PLL1CONbits.FOUTSWEN = 1u;
    n = DIVSW_WAIT_LIMIT;
    while (PLL1CONbits.FOUTSWEN && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_FOUTSWEN; }
    n = DIVSW_WAIT_LIMIT;
    while (!OSCCTRLbits.PLL1RDY && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_PLLRDY; }
    n = DIVSW_WAIT_LIMIT;
    while (!CLK6CONbits.CLKRDY && (--n != 0u)) { }
    return (n != 0u) ? CLKDIV_OK : CLKDIV_CLKRDY;
#endif
}

uint32_t clock_adc_pll_postdiv1(void) { return PLL1DIVbits.POSTDIV1; }
uint32_t clock_adc_pll_postdiv2(void) { return PLL1DIVbits.POSTDIV2; }

uint32_t clock_adc_hz(void)
{
    /* Derived from the registers, not from a constant: FVCO = FRC *
     * PLLFBDIV / PLLPRE, output = FVCO / (POSTDIV1 * POSTDIV2), then the
     * CLKGEN6 divide ratio. A switch that did not take is then visible
     * here instead of being papered over by an assumed 320 MHz. */
    return (uint32_t)(((uint64_t)pll1_out_hz() * 100u) / clock_adc_div());
}

uint32_t clock_adc_set_pll(uint32_t postdiv1, uint32_t postdiv2)
{
    /* Both fields are three bits, and POSTDIV1 must not be smaller than
     * POSTDIV2 (p778). 7/7 gives 32.65 MHz, just above the ADC's 32 MHz
     * minimum (Table 16-1); 5/1 is the 320 MHz clock_init() sets up. */
    if ((postdiv1 < 1u) || (postdiv1 > 7u) ||
        (postdiv2 < 1u) || (postdiv2 > 7u) || (postdiv2 > postdiv1)) {
        return CLKDIV_RANGE;
    }
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return CLKDIV_OK;                  /* no PLL to retune                */
#else
    /* "The output dividers POSTDIV1 and POSTDIV2 should not be changed
     * while the PLL is operating" (p778) - the caller has taken the ADC
     * core down, which is what consumes this clock. The update itself is
     * the same FOUTSWEN step clock_init() uses at boot, and that one
     * demonstrably works: the board would not start otherwise. */
    PLL1DIVbits.POSTDIV1 = postdiv1;
    PLL1DIVbits.POSTDIV2 = postdiv2;
    if ((PLL1DIVbits.POSTDIV1 != postdiv1) || (PLL1DIVbits.POSTDIV2 != postdiv2)) {
        return CLKDIV_NOT_WRITTEN;
    }
    PLL1CONbits.FOUTSWEN = 1u;         /* apply the output dividers       */
    uint32_t n = DIVSW_WAIT_LIMIT;
    while (PLL1CONbits.FOUTSWEN && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_FOUTSWEN; }
    n = DIVSW_WAIT_LIMIT;
    while (!OSCCTRLbits.PLL1RDY && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_PLLRDY; }
    n = DIVSW_WAIT_LIMIT;
    while (!CLK6CONbits.CLKRDY && (--n != 0u)) { }
    if (n == 0u) { return CLKDIV_CLKRDY; }
    if ((PLL1DIVbits.POSTDIV1 != postdiv1) || (PLL1DIVbits.POSTDIV2 != postdiv2)) {
        return CLKDIV_LOST;
    }
    return CLKDIV_OK;
#endif
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
    console_kv_hex("CLK7DIV", CLK7DIV);
    console_kv_hex("VCO1DIV", VCO1DIV);     /* DAC clock source, 400 MHz */
    console_kv_hex("CLK13CON", CLK13CON);   /* SCCP1 trigger clock       */
    console_kv_hex("CLK13DIV", CLK13DIV);
    console_kv_hex("IEC0", IEC0);           /* CLKFAIL enable, bit 9     */
    console_kv_hex("IFS0", IFS0);           /* CLKFAIL flag,   bit 9     */
}

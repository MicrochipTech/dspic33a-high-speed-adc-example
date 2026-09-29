/*
 * sccp.c - SCCP1 as the ADC's sample-rate generator (see sccp.h for what
 * was wrong here until 24.09.2026 and why every setting is a parameter)
 *
 * Register layout from the device pack's ATDF, which is the source to
 * trust for this module - the datasheet's trigger table disagrees with it
 * about which number selects SCCP1, and the board sided with the ATDF:
 *
 *   CCP1CON1: MOD[3:0] 0xf, CCSEL 0x10, T32 0x20, TMRPS 0xc0,
 *             CLKSEL[10:8] 0x700, ON 0x8000, SYNC[20:16], TRIGEN 0x800000,
 *             OPS[27:24], OPSSRC 0x80000000
 *   CCP1CON2: AUXOUT[20:19] 0x180000, OCAEN 0x1000000
 *   CCP1PR:   32-bit period      CCP1RA/RB: compare registers
 *
 * MOD 0 is the 16/32-bit timer; MOD 1 with CCSEL 0 is "32-Bit Single
 * Edge, High", the plainest output-compare mode. T32 = 1 makes the timer
 * and the compare registers 32 bits wide, which is what gives the period
 * its range.
 */

#include <xc.h>
#include "sccp.h"
#include "clock.h"
#include "regs.h"       /* port layer (src/port): reg_visit_t for the register dump (P4.8) */

static uint32_t   sccp1_ticks = 0;
static sccp_clk_t sccp1_clk   = SCCP_CLK_PERIPHERAL;

bool sccp1_start(uint32_t ticks, sccp_clk_t clk, sccp_mode_t mode, sccp_event_t ev)
{
    if (ticks < 2u) { return false; }

    CCP1CON1bits.ON = 0u;             /* down before anything is changed */
    CCP1CON1 = 0u;
    CCP1CON2 = 0u;

    CCP1CON1bits.CLKSEL = (uint32_t)clk;
    CCP1CON1bits.T32    = 1u;         /* 32-bit timer and compare        */
    /* The compare point inside the period, written in BOTH modes. Half
     * way, so an event there is as far from both period edges as it can
     * be - if the event and the rollover were to coincide, a synchroniser
     * could swallow one of them. Until 25.09.2026 it was written in OC
     * mode only, so a timer-mode start after an OC run with a longer
     * period inherited a CCP1RB beyond the new CCP1PR: a compare that
     * never matches. */
    CCP1RA = 0u;
    CCP1RB = ticks / 2u;
    if (mode == SCCP_MODE_OC) {
        CCP1CON1bits.CCSEL = 0u;      /* output compare, not capture     */
        CCP1CON1bits.MOD   = 1u;      /* 32-bit single edge, high        */
    } else {
        CCP1CON1bits.CCSEL = 0u;
        CCP1CON1bits.MOD   = 0u;      /* 16/32-bit timer                 */
    }
    CCP1CON2bits.AUXOUT = (uint32_t)ev;

    /* The timer counts 0..PR and rolls over, so `ticks` cycles per period
     * means PR = ticks - 1. */
    CCP1PR  = ticks - 1u;
    CCP1TMR = 0u;

    sccp1_ticks = ticks;
    sccp1_clk   = clk;
    CCP1CON1bits.ON = 1u;

    /* Did the settings survive the write? The module has surprised us
     * before; a readback costs nothing and turns a silent misconfiguration
     * into a visible one. */
    return (CCP1CON1bits.ON != 0u) &&
           (CCP1CON1bits.CLKSEL == (uint32_t)clk) &&
           (CCP1CON2bits.AUXOUT == (uint32_t)ev) &&
           (CCP1PR == (ticks - 1u));
}

void sccp1_stop(void)
{
    CCP1CON1bits.ON = 0u;
}

uint32_t sccp1_tmr(void) { return CCP1TMR; }

/* ------------------------------------------------------------------ *
 * Counting the module's events with the CPU (low rates only)
 *
 * SCCP1 has two interrupts (DS70005591D interrupt vector table, "CCP 1
 * Timer" and "CCP 1 Input"): _CCT1Interrupt, IRQ 51, IFS1/IEC1 bit 19,
 * priority IPC6[14:12] - the timer period event; and _CCP1Interrupt,
 * IRQ 52, bit 20, IPC6[18:16] - the capture/compare event. Timer mode
 * raises the first, output-compare mode the second, so both are counted
 * and reported apart. Which of them coincides with the special event
 * the ADC listens to is exactly what the chain test's S2 compares
 * against the number of ADC results.
 *
 * Priority 3: above the console's receive interrupt (1), inside which
 * every test runs, below the DMA (4). At 100 kHz each entry costs well
 * under a microsecond.
 * ------------------------------------------------------------------ */
#define SCCP1_COUNT_PRIORITY  3u
volatile uint32_t sccp1_timer_events = 0;
volatile uint32_t sccp1_cmp_events   = 0;

void sccp1_count(bool on)
{
    IEC1bits.CCT1IE = 0u;
    IEC1bits.CCP1IE = 0u;
    IFS1bits.CCT1IF = 0u;
    IFS1bits.CCP1IF = 0u;
    sccp1_timer_events = 0u;
    sccp1_cmp_events   = 0u;
    if (on) {
        IPC6bits.CCT1IP = SCCP1_COUNT_PRIORITY;
        IPC6bits.CCP1IP = SCCP1_COUNT_PRIORITY;
        IEC1bits.CCT1IE = 1u;
        IEC1bits.CCP1IE = 1u;
    }
}

void __attribute__((interrupt, no_auto_psv)) _CCT1Interrupt(void)
{
    IFS1bits.CCT1IF = 0u;             /* first, see dma.c                */
    sccp1_timer_events++;
}

void __attribute__((interrupt, no_auto_psv)) _CCP1Interrupt(void)
{
    IFS1bits.CCP1IF = 0u;
    sccp1_cmp_events++;
}

uint32_t sccp1_hz(void)
{
    /* Read back, never assumed: the peripheral clock comes from PLL2 and
     * CLKGEN13 from PLL1, and the whole point of the CLKGEN13 option is
     * that the ADC and its trigger share a source. If the generator did
     * not come up this returns its real (wrong) value and the nominal
     * rate below is visibly off. */
    return (sccp1_clk == SCCP_CLK_GEN13) ? clock_trig_hz()
                                         : clock_periph_hz();
}

uint32_t sccp1_period(void) { return sccp1_ticks; }

uint32_t sccp1_nominal_ksps(void)
{
    return (sccp1_ticks != 0u) ? (sccp1_hz() / 1000u / sccp1_ticks) : 0u;
}

/* ------------------------------------------------------------------ *
 * SCCP2: the signal generator's playback clock (SG.2, 29.09.2026)
 *
 * Same register layout as SCCP1 (pack header p33AK512MPS512.h: CCP2CON1
 * MOD[3:0], CCSEL, T32, TMRPS[7:6], CLKSEL[10:8], ON bit 15; CCP2PR as
 * PRL[15:0]/PRH[31:16]; CCP2CON2 AUXOUT[20:19]) - checked, not assumed.
 *
 * What it paces is DMA channel 1, CHSEL 0x19 "CCP2 IC/OC" (Table 13-2
 * p797): the module's IC/OC event, CCP2IF - not its timer event CCT2IF
 * (ATDF: vector 53 "timer interrupt", 54 "input capture or output
 * compare interrupt"). In the plain 32-bit timer (T32 = 1, Fig. 26-4) a
 * period match sets CCTxIF only, so that mode would pace nothing. Two
 * ways to get CCP2IF once per period, both kept, chosen by the caller
 * until a board run has said which one the DMA sees
 * (docs/IMPLEMENTATION-PLAN.md SG.8):
 *   SCCP2_PACE_TMR16  dual 16-bit timer (T32 = 0, MOD = 0): the
 *                     secondary timer's rollover "generates a timer
 *                     rollover interrupt event (CCPxIF)" (26.x, p1781,
 *                     Fig. 26-3). Period in PRH, 16 bits, so the
 *                     prescaler TMRPS (1:1/4/16/64) stretches it.
 *   SCCP2_PACE_OC32   32-bit output compare, single edge (T32 = 1, MOD = 1,
 *                     as SCCP1's OC mode): the compare match on CCP2RB is
 *                     the OC event. Run 18 saw SCCP1's OC mode give the
 *                     ADC no events - through AUXOUT, which is not this
 *                     path - so it is a candidate, not a certainty.
 * Clock: the standard peripheral clock (CLKSEL = 000, Table 26-2 p1756;
 * CPU clock / 2 = 100 MHz here, clock_periph_hz()), not CLKGEN13: the
 * generator has no reason to share PLL1 with the ADC, and a play rate
 * that moved with every "stream on" would be exactly the coupling that
 * cost runs 5-7. 100 MHz is inside Table 40-24's 200 MHz limit (p2016).
 * ------------------------------------------------------------------ */
static uint32_t       sccp2_ticks_ = 0u;          /* period, input clocks */
static uint32_t       sccp2_div_   = 1u;          /* prescaler            */
static sccp2_pace_t   sccp2_pace_  = SCCP2_PACE_TMR16;

bool sccp2_start(uint32_t ticks, sccp2_pace_t pace)
{
    if (ticks < 2u) { return false; }
    /* TMRPS: 00 = 1:1, 01 = 1:4, 10 = 1:16, 11 = 1:64 (CCPxCON1 TMRPS). */
    uint32_t ps = 0u, div = 1u, t = ticks;
    if (pace == SCCP2_PACE_TMR16) {
        while ((t > 65536u) && (ps < 3u)) { ps++; div *= 4u; t = (ticks + div / 2u) / div; }
        if (t > 65536u) { return false; }
        if (t < 2u)     { t = 2u; }
    }

    CCP2CON1bits.ON = 0u;
    CCP2CON1 = 0u;
    CCP2CON2 = 0u;
    CCP2CON1bits.CLKSEL = (uint32_t)SCCP_CLK_PERIPHERAL;
    CCP2CON1bits.TMRPS  = ps;
    CCP2CON1bits.CCSEL  = 0u;
    CCP2TMR = 0u;
    if (pace == SCCP2_PACE_TMR16) {
        CCP2CON1bits.T32 = 0u;        /* dual 16-bit timer               */
        CCP2CON1bits.MOD = 0u;
        /* Primary timer at its longest; the secondary counts the period
         * whose rollover is the CCP2IF event. */
        CCP2PR = ((t - 1u) << 16) | 0xFFFFu;
        CCP2RA = 0u;
        CCP2RB = 0u;
    } else {
        CCP2CON1bits.T32 = 1u;        /* 32-bit timer and compare        */
        CCP2CON1bits.MOD = 1u;        /* 32-bit single edge, high        */
        CCP2PR = t - 1u;
        CCP2RA = 0u;
        CCP2RB = t / 2u;              /* half way, as SCCP1              */
    }
    CCP2CON2bits.AUXOUT = (uint32_t)SCCP_EVENT_SPECIAL;
    sccp2_ticks_ = t;
    sccp2_div_   = div;
    sccp2_pace_  = pace;
    CCP2CON1bits.ON = 1u;
    return (CCP2CON1bits.ON != 0u) && (CCP2CON1bits.TMRPS == ps) &&
           (CCP2CON1bits.T32 == ((pace == SCCP2_PACE_TMR16) ? 0u : 1u));
}

void sccp2_stop(void)
{
    CCP2CON1bits.ON = 0u;
    sccp2_ticks_ = 0u;
}

uint32_t sccp2_hz(void)          { return clock_periph_hz(); }
uint32_t sccp2_period(void)      { return sccp2_ticks_ * sccp2_div_; }
sccp2_pace_t sccp2_pace(void)    { return sccp2_pace_; }

uint32_t sccp2_actual_hz(void)
{
    const uint32_t p = sccp2_period();
    return (p != 0u) ? (uint32_t)(((uint64_t)sccp2_hz() + p / 2u) / p) : 0u;
}

/* The module's two interrupt flags, with the interrupts left disabled -
 * the flags are set either way. Bit 0 = CCT2IF (timer), bit 1 = CCP2IF
 * (IC/OC, the DMA's trigger). Clear, wait a while, read. */
void sccp2_flags_clear(void)
{
    IFS1bits.CCT2IF = 0u;
    IFS1bits.CCP2IF = 0u;
}

uint32_t sccp2_flags_read(void)
{
    return (IFS1bits.CCT2IF ? 1u : 0u) | (IFS1bits.CCP2IF ? 2u : 0u);
}

void sccp2_regs_visit(reg_visit_t visit)
{
    const uint32_t t1 = CCP2TMR;
    const uint32_t t2 = CCP2TMR;
    visit("[regs] sccp2\r\n", 0u, REG_TITLE);
    visit("CCP2CON1", CCP2CON1, REG_HEX);
    visit("CCP2CON2", CCP2CON2, REG_HEX);
    visit("CCP2PR", CCP2PR, REG_HEX);
    visit("CCP2RB", CCP2RB, REG_HEX);
    visit("CCP2TMR (first read)", t1, REG_HEX);
    visit("CCP2TMR (second read)", t2, REG_HEX);
    visit("pace (0 tmr16, 1 oc32)", (uint32_t)sccp2_pace_, REG_DEC);
    visit("prescaler", sccp2_div_, REG_DEC);
    visit("module clock Hz", sccp2_hz(), REG_DEC);
    visit("play Hz actual", sccp2_actual_hz(), REG_DEC);
}

void sccp1_regs_visit(reg_visit_t visit)
{
    /* Two reads of the counter a few instructions apart: both 0 means the
     * module never started, and then no trigger can reach the ADC
     * whatever the trigger source selects. Two equal non-zero reads are
     * not proof of a stopped timer - with a short period it wraps between
     * the reads - so the raw values are printed and not judged here. */
    const uint32_t t1 = CCP1TMR;
    const uint32_t t2 = CCP1TMR;
    visit("[regs] sccp1\r\n", 0u, REG_TITLE);
    visit("CCP1CON1", CCP1CON1, REG_HEX);
    visit("CCP1CON2", CCP1CON2, REG_HEX);
    visit("CCP1PR", CCP1PR, REG_HEX);
    visit("CLKSEL (0 periph, 1 CLKGEN13)", CCP1CON1bits.CLKSEL, REG_DEC);
    visit("MOD", CCP1CON1bits.MOD, REG_DEC);
    visit("AUXOUT (1 rollover, 2 special event)", CCP1CON2bits.AUXOUT, REG_DEC);
    visit("CCP1TMR (first read)", t1, REG_DEC);
    visit("CCP1TMR (second read)", t2, REG_DEC);
    visit("CCP1RA", CCP1RA, REG_HEX);
    visit("CCP1RB", CCP1RB, REG_HEX);
    visit("module clock Hz", sccp1_hz(), REG_DEC);
    visit("nominal ksps", sccp1_nominal_ksps(), REG_DEC);
}

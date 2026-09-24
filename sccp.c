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
#include "console.h"

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

void sccp1_regs_dump(void)
{
    /* Two reads of the counter a few instructions apart: both 0 means the
     * module never started, and then no trigger can reach the ADC
     * whatever the trigger source selects. Two equal non-zero reads are
     * not proof of a stopped timer - with a short period it wraps between
     * the reads - so the raw values are printed and not judged here. */
    const uint32_t t1 = CCP1TMR;
    const uint32_t t2 = CCP1TMR;
    console_puts("[regs] sccp1\r\n");
    console_kv_hex("CCP1CON1", CCP1CON1);
    console_kv_hex("CCP1CON2", CCP1CON2);
    console_kv_hex("CCP1PR", CCP1PR);
    console_kv("CLKSEL (0 periph, 1 CLKGEN13)", CCP1CON1bits.CLKSEL);
    console_kv("MOD", CCP1CON1bits.MOD);
    console_kv("AUXOUT (1 rollover, 2 special event)", CCP1CON2bits.AUXOUT);
    console_kv("CCP1TMR (first read)", t1);
    console_kv("CCP1TMR (second read)", t2);
    console_kv_hex("CCP1RA", CCP1RA);
    console_kv_hex("CCP1RB", CCP1RB);
    console_kv("module clock Hz", sccp1_hz());
    console_kv("nominal ksps", sccp1_nominal_ksps());
}

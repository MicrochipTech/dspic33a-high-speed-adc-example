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
    if (mode == SCCP_MODE_OC) {
        CCP1CON1bits.CCSEL = 0u;      /* output compare, not capture     */
        CCP1CON1bits.MOD   = 1u;      /* 32-bit single edge, high        */
        /* The compare point inside the period. Half way, so the event is
         * as far from both period edges as it can be - if the event and
         * the rollover were to coincide, a synchroniser could swallow
         * one of them. */
        CCP1RA = 0u;
        CCP1RB = ticks / 2u;
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
    console_kv("module clock Hz", sccp1_hz());
    console_kv("nominal ksps", sccp1_nominal_ksps());
}

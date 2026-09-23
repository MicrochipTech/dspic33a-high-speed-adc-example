/*
 * sccp.c - SCCP1 in timer mode as the ADC's pacing clock (see sccp.h)
 *
 * Register layout from the device header (CCP1CON1BITS): MOD[3:0] = 0
 * is the 16/32-bit timer mode, T32 selects 32 bits, CLKSEL = 0 is the
 * standard speed peripheral clock (MCC: "CLKSEL Standard Speed
 * Peripheral Clock" for that value), TMRPS = 0 is prescaler 1:1, ON is
 * bit 15. The trigger to the ADC is the period match; Example 16-8 sets
 * nothing else (no OPS/SYNC bits), so neither does this.
 *
 * Period: the timer counts 0..PR and rolls over, so `ticks` cycles per
 * period means PR = ticks - 1. The datasheet example writes FCY / f
 * without the -1; for the rates here the difference is what the rate
 * test measures, not what the code assumes.
 */

#include <xc.h>
#include "sccp.h"

static uint32_t sccp1_ticks = 0;

void sccp1_start(uint32_t ticks)
{
    if (ticks < 2u) { ticks = 2u; }
    CCP1CON1bits.ON = 0u;
    CCP1CON1 = 0u;                    /* MOD 0 timer, CLKSEL 0, TMRPS 0 */
    CCP1CON2 = 0u;
    CCP1CON1bits.T32 = 1u;            /* 32-bit                          */
    CCP1PR  = ticks - 1u;
    CCP1TMR = 0u;
    sccp1_ticks = ticks;
    CCP1CON1bits.ON = 1u;
}

void sccp1_stop(void)
{
    CCP1CON1bits.ON = 0u;
}

uint32_t sccp1_period(void)
{
    return sccp1_ticks;
}

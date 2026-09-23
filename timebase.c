/*
 * timebase.c - Timer1 as a free-running 12.5 MHz counter (see timebase.h)
 *
 * T1CON (device header, T1CONBITS): TCS = 0 internal clock, TCKPS = 1 is
 * prescaler 1:8, ON bit 15. TMR1 and PR1 are 32-bit on this device.
 */

#include <xc.h>
#include <libpic30.h>       /* __delay32()                          */
#include "timebase.h"

void timebase_init(void)
{
    if (T1CONbits.ON) {
        return;                       /* already running: keep counting  */
    }
    T1CON = 0u;                       /* off, internal clock, no gate    */
    TMR1  = 0u;
    PR1   = 0xFFFFFFFFu;              /* free running, 32 bit            */
    T1CONbits.TCKPS = 1u;             /* 1:8                             */
    T1CONbits.ON    = 1u;
}

uint32_t timebase_ticks(void)
{
    return TMR1;
}

uint32_t timebase_check(void)
{
    timebase_init();
    const uint32_t t0 = TMR1;
    __delay32(20000000ul);            /* 100 ms at 200 MHz               */
    return TMR1 - t0;
}

uint32_t timebase_ksps(uint32_t samples, uint32_t ticks)
{
    if (ticks == 0u) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)samples * TIMEBASE_HZ / 1000u) / ticks);
}

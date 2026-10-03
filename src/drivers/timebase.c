/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

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
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    /* The simulator has no real time base to check and executes this
     * loop at a small fraction of real time: 20 M cycles took longer
     * than the whole test budget (24.09.2026, the boot "hung" here).
     * 1 % of the delay keeps the call and its print, the value means
     * nothing there anyway. */
    __delay32(200000ul);
#else
    __delay32(20000000ul);            /* 100 ms at 200 MHz               */
#endif
    return TMR1 - t0;
}

uint32_t timebase_ksps(uint32_t samples, uint32_t ticks)
{
    if (ticks == 0u) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)samples * TIMEBASE_HZ / 1000u) / ticks);
}

/*
 * sccp.h - SCCP1 as a timer whose period match triggers the ADC (sccp.c)
 *
 * The second of the three ways to pace the conversions (capture.c,
 * "pacing"): a plain 32-bit timer on the standard peripheral clock
 * (100 MHz, 10 ns per tick), period match = ADC trigger, TRG2SRC = 32
 * "SCCP1 trigger" (DS70005591D Table 16-4, p1227). This is what
 * datasheet Example 16-8 (p1334) does together with Integration mode:
 * CCP1CON1.MOD = 0, T32 = 1, CCP1PR = FCY / f, ON = 1. Nothing else.
 */
#ifndef SCCP_H
#define SCCP_H

#include <stdint.h>

#define SCCP_TICK_HZ   100000000u   /* standard peripheral clock        */

/* Timer mode, 32-bit, period `ticks` (the trigger fires every `ticks`
 * clock cycles), running. Safe to call again with a new period. */
void sccp1_start(uint32_t ticks);
void sccp1_stop(void);
uint32_t sccp1_period(void);        /* ticks, as configured           */

#endif /* SCCP_H */

/*
 * timebase.h - a free-running counter to measure rates against (timebase.c)
 *
 * Timer1, 32-bit, on the peripheral clock with prescaler 1:8: 12.5 MHz,
 * 80 ns per tick, wraps after 343 s. Independent of the ADC clock, which
 * is the point: the sample rate the ADC delivers is judged against this,
 * not against what its own registers claim. timebase_check() measures
 * the tick rate itself against the CPU clock once, so an assumption
 * about the timer's clock cannot silently scale every result.
 */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

#define TIMEBASE_HZ   12500000u     /* 100 MHz peripheral clock / 8 */

/* Start the counter (idempotent). */
void timebase_init(void);

/* Current count; differences are wrap-safe in uint32_t arithmetic. */
uint32_t timebase_ticks(void);

/* Ticks counted during 100 ms of CPU time (__delay32 at 200 MHz):
 * 1 250 000 if the clock assumption holds. */
uint32_t timebase_check(void);

/* samples / (ticks / TIMEBASE_HZ) in kSPS, 64-bit inside. 0 for ticks 0. */
uint32_t timebase_ksps(uint32_t samples, uint32_t ticks);

#endif /* TIMEBASE_H */

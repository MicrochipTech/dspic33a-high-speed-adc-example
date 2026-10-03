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

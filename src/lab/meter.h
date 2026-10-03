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
 * meter.h - the back-to-back measurement instruments (meter.c)
 *
 * Moved out of capture.c on 27.09.2026 (P9.3, docs/IMPLEMENTATION-PLAN.md):
 * one half processed with nothing else running, the self-test on the
 * ADC's internal reference, the CLKGEN6 control experiment, the one-shot
 * burst(s), and the delivered-rate measurement. All of them are blocking
 * and bounded, print through console.h exactly as before the move, and
 * are otherwise unchanged - see meter.c.
 *
 * Declared here rather than in capture.h so that this file, not
 * capture.c, is what documents them; capture.h pulls this header in with
 * one #include so every existing caller of capture.h (cli.c, bench.c,
 * gui_link.c, chaintest.c, dactest.c) keeps working unchanged.
 */
#ifndef METER_H
#define METER_H

#include <stdint.h>

/* One half processed with the DMA idle, in Timer1 ticks (80 ns). */
uint32_t capture_process_bench(void);

/* Sample the ADC's internal 15/16 * VDD reference (ANx6) for a few halves
 * and compare the mean against the expected window. Blocking, bounded.
 * Returns 0 on success, 6 if no data arrived, 7 if the mean is outside the
 * window, 8 if the DMA channel switched itself off. The mean is stored in
 * selftest_mean and returned through *mean if non-NULL. Restores the
 * previous input afterwards. */
uint32_t capture_selftest(uint32_t *mean);

/* Switch CLKGEN6 off and try to convert anyway: the control experiment
 * for "is the ADC really clocked from CLKGEN6?". 0 means halves still
 * arrived with the generator off, anything else that nothing did. The
 * generator and the ADC core are restored either way. Blocking, bounded.
 * In the simulator it returns 6 without doing anything. */
uint32_t capture_clkoff_probe(uint32_t halves);

/* Run `halves` halves at whatever the divider is set to and return the
 * delivered rate in ksps, measured against Timer1. Blocking, bounded,
 * prints nothing - the caller judges and reports. Returns 0, or 6/8 from
 * the waits. In the simulator it returns 0 with ksps 0. */
uint32_t capture_measure_rate(uint32_t halves, uint32_t *ksps);

/* Fill the buffer exactly once and stop, the stop decided in the DMA
 * interrupt. Afterwards the whole buffer - 2 * capture_half_len()
 * samples from capture_buffer() - is one contiguous window that nothing
 * is writing any more. This is the only way to look at the data at a
 * rate where the main loop runs tens of milliseconds behind the DMA.
 * Returns 0, or 6/8 from the wait. */
uint32_t capture_oneshot(void);
/* The same, but `bursts` bursts back to back before it stops - the ISR
 * restarts each one exactly as continuous streaming does, and only the
 * last ends the run. It exists to answer the contradiction of run 16: a
 * single burst delivers the rate the PLL was set to, while a run of a
 * thousand delivers about 40 MSPS whatever the setting. If the rate
 * measured over ten bursts equals the rate over one, the first burst is
 * ordinary and the difference lies in continuous operation; if it jumps,
 * the first burst is the odd one and every "clean" rate measured so far
 * describes a start-up, not the stream. */
uint32_t capture_oneshot_n(uint32_t bursts);

#endif /* METER_H */

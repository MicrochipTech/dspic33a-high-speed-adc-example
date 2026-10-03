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
 * wavegen.h - synthesis of a signal-generator table (wavegen.c)
 *
 * The formula of waveform_generator/firmware/src/tab_wave_gen.py,
 * generate_wave() lines 35-63, without the GUI, WAV export and plot
 * (DESIGN-MULTICHANNEL.md 4.2, 27.09.2026, P3.6):
 *
 *   t_i = i / play_hz,  i = 0 .. n-1
 *   y_i = exp(-decay t_i) x sum_k a_k sin(2 pi (k+1) f0 t_i),  k = 0..6,
 *         a_0 = 1, a_1..a_6 = harm[0..5] (2nd .. 7th harmonic)
 *   table_i = out_min + round((y_i - min y) / (max y - min y)
 *                             x (out_max - out_min) x amplitude)
 *
 * so the minimum lands on out_min and the maximum on out_min +
 * (out_max - out_min) x amplitude; with out_min = 0, out_max = 1023 that
 * is the script's 0 .. 1023 x amplitude. The script multiplies every
 * harmonic by amplitude as well (lines 37-43); that cancels in the
 * normalisation and is left out here.
 *
 * Float on the target, on purpose: the table is computed once per
 * parameter change, not per sample; the dsPIC33A has an FPU; sinf/expf
 * come from libm; and the reference model (tests/ref/wavegen_ref.py) is
 * float64, against which a float32 table lands within +-1 LSB (the host
 * test measures it). A fixed-point sine would need its own table and its
 * own accuracy argument for nothing. The whole table is computed twice
 * (once for min/max, once for the values) rather than kept in a float
 * scratch buffer of n x 4 bytes.
 *
 * The float32 phase argument 2 pi (k+1) f0 t grows with the table: at
 * 7 x f0 x n / play_hz total periods of the 7th harmonic the argument's
 * rounding is about that many x 4e-7 rad; up to a few hundred periods
 * (the P3.1 vectors: 72) that is well under 0.1 LSB of a 12-bit table.
 *
 * Option `snap`: the table is played cyclically, and unless
 * f0 x n / play_hz is a whole number the signal jumps at the wrap. With
 * snap the fundamental is moved to the nearest whole number of periods
 * (at least 1), wavegen_snap_hz() says where, and wavegen_fill() reports
 * it in *f0_used. With decay the table is one pulse and the wrap does not
 * matter, but snap is still honoured.
 *
 * wavegen_fill() checks its inputs and returns a code instead of a table
 * for: n < 2, play_hz = 0, f0 outside (0, play_hz / 2), decay < 0,
 * amplitude outside (0, 1] (0 would divide by zero in the script's
 * formula), out_min >= out_max, and a flat signal (max y = min y - e.g.
 * a decay so large that every sample after the first is 0).
 */
#ifndef WAVEGEN_H
#define WAVEGEN_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t n;            /* table size (samples), >= 2 */
    uint32_t play_hz;      /* playback rate */
    float    f0_hz;        /* fundamental, 0 < f0 < play_hz / 2 */
    float    harm[6];      /* 2nd .. 7th harmonic as a factor of the fundamental */
    float    decay;        /* envelope exp(-decay t), >= 0 (0 = none) */
    float    amplitude;    /* 0 < amplitude <= 1 */
    uint16_t out_min;      /* output range, for the DAC 205 .. 3890 (ATDF) */
    uint16_t out_max;
} wavegen_cfg_t;

typedef enum {
    WAVEGEN_OK = 0,
    WAVEGEN_E_N,           /* n < 2 */
    WAVEGEN_E_RATE,        /* play_hz = 0 */
    WAVEGEN_E_F0,          /* f0 <= 0 or f0 >= play_hz / 2 */
    WAVEGEN_E_DECAY,       /* decay < 0 */
    WAVEGEN_E_AMPLITUDE,   /* amplitude <= 0 or > 1 */
    WAVEGEN_E_RANGE,       /* out_min >= out_max */
    WAVEGEN_E_FLAT         /* the signal has no swing to scale */
} wavegen_result_t;

/* The fundamental snapped to the nearest whole number of periods in the
 * table (at least one): round(f0 n / play_hz) x play_hz / n. */
float wavegen_snap_hz(const wavegen_cfg_t *c);

/* Fill table[0..n) from c. With snap the fundamental is
 * wavegen_snap_hz(c) instead of c->f0_hz; the frequency used is written
 * to *f0_used when that is not NULL (also without snap: then it is
 * c->f0_hz). On an error code the table is untouched. */
wavegen_result_t wavegen_fill(const wavegen_cfg_t *c, uint16_t *table,
                              bool snap, float *f0_used);

#endif /* WAVEGEN_H */

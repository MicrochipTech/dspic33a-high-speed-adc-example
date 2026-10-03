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
 * goertzel_f.h - damped Goertzel resonator in float, one instance per
 *                channel and frequency (goertzel_f.c)
 *
 * Taken over on 27.09.2026 (P3.3) from the template
 * Goertzel/goertzel/firmware/src/goertzel.c, Goertzel_f_Init() /
 * Goertzel_f_Filter() (lines 97-155), with these changes, all decided in
 * DESIGN-MULTICHANNEL.md 4.3:
 *
 *   - the recurrence is the textbook damped Goertzel (user decision
 *     27.09.2026):  q0 = s + 2 D cos(w) q1 - D^2 q2;  q2 = q1;  q1 = q0.
 *     Poles at D e^(+-jw): radius D, centre exactly w. The template's
 *     float variant multiplied D in three places (lines 130-132), which
 *     damps but detunes the centre to cos(theta) = D cos(w);
 *   - real/imag (135-136) and the magnitude approximation
 *     |re| + |im| - min(|re|, |im|) / 2 (137) are the template's;
 *   - the magnitude is truncated to int32 (units of the shifted input
 *     sample) and low-passed by a lib/iir1 instance with k =
 *     GOERTZEL_LP_K, per instance - the template's global iIIR_Tap[] and
 *     its `magnitude >> 16` (line 140, which left the float variant's
 *     output at 0..3) are gone;
 *   - `window` (the template's WINDOW_SIZE) decimates the OUTPUT: the
 *     low-pass runs every sample, and every `window`-th sample its value
 *     is written to mag_out. window = 1 writes every sample;
 *   - block length n, in_shift (the template's fixed >> 2) and damping are
 *     parameters; no static state (the template's static down_counter is
 *     win_cnt in the instance);
 *   - the detector is NOT in here. The template reset the resonator on a
 *     detection inside the filter loop (lines 143-152); the decided
 *     detector (fire once, re-arm by hysteresis) needs no reset, so it is
 *     a second pass over the magnitudes this function writes: lib/detect
 *     (P3.5) consumes mag_out. This keeps the module free of a dependency
 *     on a module that does not exist yet, at the cost of one int32 per
 *     emitted magnitude in the caller's buffer.
 *
 * mag_out must hold ceil(n / window) values; the return value says how
 * many were written (win_cnt carries across blocks, so successive blocks
 * of the same stream give exactly the same magnitudes as one big block).
 *
 * Reference model: tests/ref/goertzel_ref.py (its `damped` form) -
 * float64 in the resonator, exact integers after it. The host test
 * tests/host/test_goertzel_f.c compares against its vectors with a
 * tolerance for float32.
 */
#ifndef GOERTZEL_F_H
#define GOERTZEL_F_H

#include <stdint.h>
#include "iir1.h"

/* The low-pass on the magnitude: the template's LP_SHIFT_VALUE_K. */
#define GOERTZEL_LP_K 4u

typedef struct {
    float    coeff;     /* 2 D cos(w) */
    float    dd;        /* D^2 */
    float    cos_t;     /* cos(w), for the real part */
    float    sin_t;     /* sin(w), for the imaginary part */
    float    q1, q2;    /* the resonator state */
    iir1_t   lp;        /* low-pass on the truncated magnitude */
    uint32_t window;    /* emit every window-th low-pass value (>= 1) */
    uint32_t win_cnt;   /* samples since the last emitted value */
    uint8_t  in_shift;  /* x >> in_shift before the resonator */
} goertzel_f_t;

/* Coefficients for f_hz at fs_hz with damping D (0 < D < 1, 0.995 in the
 * template), output every `window` samples (0 counts as 1), input shifted
 * right by in_shift (the template: 2, 12-bit ADC -> 10 bit). Clears the
 * state. */
void goertzel_f_init(goertzel_f_t *g, float fs_hz, float f_hz, float damping,
                     uint32_t window, uint8_t in_shift);

/* Clear the state (resonator, low-pass, window counter), keep the
 * coefficients. */
void goertzel_f_reset(goertzel_f_t *g);

/* Run n samples through the resonator and low-pass; write every
 * window-th low-passed magnitude to mag_out. Returns the count written. */
uint32_t goertzel_f_block(goertzel_f_t *g, const uint16_t *x, uint32_t n,
                          int32_t *mag_out);

#endif /* GOERTZEL_F_H */

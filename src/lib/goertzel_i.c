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
 * goertzel_i.c - damped Goertzel resonator in fixed point (see goertzel_i.h)
 *
 * Goertzel_i_Init() / Goertzel_i_Filter() of the template (goertzel.c
 * lines 13-89) with the decided recurrence, Q12 state, rounded Q16
 * coefficients, the per-instance low-pass and the output decimation.
 */

#include <math.h>
#include "goertzel_i.h"

/* 2 pi as a float literal: M_PI is not in C11's math.h. */
#define GOERTZEL_TWO_PI 6.283185307179586f

/* Q16, rounded to nearest (the template truncated: (int32_t)(v * SCALE)). */
static int32_t q16(float v)
{
    return (int32_t)floorf(v * 65536.0f + 0.5f);
}

/* (a * b) >> 16 in int64, floor for negative products - template line 54. */
static int32_t mul_q16(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 16);
}

void goertzel_i_init(goertzel_i_t *g, float fs_hz, float f_hz, float damping,
                     uint32_t window, uint8_t in_shift)
{
    const float w = GOERTZEL_TWO_PI * f_hz / fs_hz;         /* template line 14 */
    const float c = cosf(w);
    g->cos_t = q16(c);
    g->sin_t = q16(sinf(w));
    g->coeff = q16(2.0f * damping * c);                     /* 2 D cos(w) */
    g->dd = q16(damping * damping);                         /* D^2 */
    g->window = (window == 0u) ? 1u : window;
    g->in_shift = in_shift;
    iir1_init(&g->lp, (uint8_t)GOERTZEL_I_LP_K);
    goertzel_i_reset(g);
}

void goertzel_i_reset(goertzel_i_t *g)
{
    g->q1 = 0;
    g->q2 = 0;
    g->win_cnt = 0u;
    iir1_reset(&g->lp);
}

uint32_t goertzel_i_block(goertzel_i_t *g, const uint16_t *x, uint32_t n,
                          int32_t *mag_out)
{
    uint32_t written = 0u;
    int32_t q1 = g->q1, q2 = g->q2;
    const int32_t coeff = g->coeff, dd = g->dd;
    const int32_t cos_t = g->cos_t, sin_t = g->sin_t;

    for (uint32_t i = 0; i < n; i++) {
        /* input >> in_shift (template line 44), then to Q12 (line 50) */
        const int32_t s = ((int32_t)x[i] >> g->in_shift) << GOERTZEL_I_STATE_SHIFT;

        /* q0 = s + 2 D cos q1 - D^2 q2;  q2 = q1;  q1 = q0 */
        const int32_t q0 = s + mul_q16(coeff, q1) - mul_q16(dd, q2);
        q2 = q1;
        q1 = q0;

        /* real/imag and the magnitude approximation, lines 63-71 */
        const int32_t re = q1 - mul_q16(q2, cos_t);
        const int32_t im = mul_q16(q2, sin_t);
        const int32_t ar = (re < 0) ? -re : re;
        const int32_t ai = (im < 0) ? -im : im;
        const int32_t mag = ar + ai - (((ar < ai) ? ar : ai) >> 1);

        /* back to units of s (line 74), low-pass, emit every window-th */
        const int32_t y = iir1_lp(&g->lp, mag >> GOERTZEL_I_STATE_SHIFT);
        if (++g->win_cnt >= g->window) {
            g->win_cnt = 0u;
            mag_out[written++] = y;
        }
    }
    g->q1 = q1;
    g->q2 = q2;
    return written;
}

/*
 * goertzel_f.c - damped Goertzel resonator in float (see goertzel_f.h)
 *
 * Goertzel_f_Init() / Goertzel_f_Filter() of the template (goertzel.c
 * lines 97-155) with the decided recurrence, the per-instance low-pass and
 * the output decimation; the magnitude approximation is the template's.
 */

#include <math.h>
#include "goertzel_f.h"

/* 2 pi as a float literal: M_PI is not in C11's math.h (MinGW hides it
 * under -std=c11, xc-dsc does not promise it). */
#define GOERTZEL_TWO_PI 6.283185307179586f

void goertzel_f_init(goertzel_f_t *g, float fs_hz, float f_hz, float damping,
                     uint32_t window, uint8_t in_shift)
{
    const float w = GOERTZEL_TWO_PI * f_hz / fs_hz;         /* template line 98 */
    g->cos_t = cosf(w);
    g->sin_t = sinf(w);
    g->coeff = 2.0f * damping * g->cos_t;                   /* 2 D cos(w) */
    g->dd = damping * damping;                              /* D^2 */
    g->window = (window == 0u) ? 1u : window;
    g->in_shift = in_shift;
    iir1_init(&g->lp, (uint8_t)GOERTZEL_LP_K);
    goertzel_f_reset(g);
}

void goertzel_f_reset(goertzel_f_t *g)
{
    g->q1 = 0.0f;
    g->q2 = 0.0f;
    g->win_cnt = 0u;
    iir1_reset(&g->lp);
}

uint32_t goertzel_f_block(goertzel_f_t *g, const uint16_t *x, uint32_t n,
                          int32_t *mag_out)
{
    uint32_t written = 0u;
    float q1 = g->q1, q2 = g->q2;
    const float coeff = g->coeff, dd = g->dd;
    const float cos_t = g->cos_t, sin_t = g->sin_t;

    for (uint32_t i = 0; i < n; i++) {
        /* input, shifted as the template's >> 2 (line 124) */
        const float s = (float)((int32_t)x[i] >> g->in_shift);

        /* q0 = s + 2 D cos q1 - D^2 q2;  q2 = q1;  q1 = q0 */
        const float q0 = s + coeff * q1 - dd * q2;
        q2 = q1;
        q1 = q0;

        /* real/imag and the magnitude approximation, lines 135-137 */
        const float re = q1 - q2 * cos_t;
        const float im = q2 * sin_t;
        const float ar = fabsf(re), ai = fabsf(im);
        const float mag = ar + ai - fminf(ar, ai) * 0.5f;

        /* truncate (mag >= 0 always), low-pass, emit every window-th */
        const int32_t y = iir1_lp(&g->lp, (int32_t)mag);
        if (++g->win_cnt >= g->window) {
            g->win_cnt = 0u;
            mag_out[written++] = y;
        }
    }
    g->q1 = q1;
    g->q2 = q2;
    return written;
}

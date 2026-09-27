/*
 * goertzel_i.h - damped Goertzel resonator in fixed point, one instance
 *                per channel and frequency (goertzel_i.c)
 *
 * The fixed-point alternative to lib/goertzel_f (P3.4, 27.09.2026): the
 * same interface, the same recurrence q0 = s + 2 D cos(w) q1 - D^2 q2
 * (user decision 27.09.2026, DESIGN-MULTICHANNEL.md 4.3), the same
 * magnitude approximation, the same lib/iir1 low-pass and output
 * decimation - see goertzel_f.h for those decisions. Taken over from the
 * template's Goertzel_i_Init() / Goertzel_i_Filter() (goertzel.c lines
 * 13-89) with these changes:
 *
 *   - the coefficients 2 D cos(w), D^2, cos(w), sin(w) are Q16
 *     (template: cos, sin, 2 cos and the damping in Q16, lines 19-21, 53),
 *     rounded to nearest instead of truncated;
 *   - the state is Q(GOERTZEL_I_STATE_SHIFT) = Q12 relative to the shifted
 *     input sample s (template: s << 14, line 50). Two bits less
 *     resolution buy headroom: |q| must stay below 2^30 so that the
 *     magnitude sum |re| + |im| fits int32. The resonator's gain for a
 *     tone at its centre is 1 / ((1 - D) x 2 sin(w)), so the requirement
 *     is |s|max x 2^12 / ((1 - D) x 2 sin(w)) < 2^30. With the template's
 *     numbers (s <= 1023 after >> 2, D = 0.995, w = 0.4 pi) the peak state
 *     is about 2^29; a D closer to 1 or a centre near DC or fs/2 (small
 *     sin(w)) reduces the allowed input;
 *   - each Q16 product is an int64 multiply shifted right by 16 (floor),
 *     as the template does (lines 54-66); the second damping stage
 *     q - (q >> 8) (lines 59-60) is gone;
 *   - the magnitude is converted back with >> GOERTZEL_I_STATE_SHIFT
 *     (template: >> 16, line 74) before the low-pass, so mag_out is in
 *     units of s like goertzel_f's.
 *
 * Reference model: tests/ref/goertzel_ref.py (float64 resonator). The
 * host test tests/host/test_goertzel_i.c states the tolerance derived
 * from the Q16 rounding of the coefficients and the truncations, and
 * prints the largest deviation seen.
 */
#ifndef GOERTZEL_I_H
#define GOERTZEL_I_H

#include <stdint.h>
#include "iir1.h"

/* The low-pass on the magnitude: the template's LP_SHIFT_VALUE_K. */
#define GOERTZEL_I_LP_K 4u

/* The state's fixed-point position relative to the shifted input sample. */
#define GOERTZEL_I_STATE_SHIFT 12

typedef struct {
    int32_t  coeff;     /* 2 D cos(w), Q16 */
    int32_t  dd;        /* D^2, Q16 */
    int32_t  cos_t;     /* cos(w), Q16 */
    int32_t  sin_t;     /* sin(w), Q16 */
    int32_t  q1, q2;    /* the resonator state, Q12 */
    iir1_t   lp;        /* low-pass on the magnitude in units of s */
    uint32_t window;    /* emit every window-th low-pass value (>= 1) */
    uint32_t win_cnt;   /* samples since the last emitted value */
    uint8_t  in_shift;  /* x >> in_shift before the resonator */
} goertzel_i_t;

/* As goertzel_f_init(): coefficients for f_hz at fs_hz with damping D,
 * output every `window` samples (0 counts as 1), input >> in_shift.
 * Clears the state. */
void goertzel_i_init(goertzel_i_t *g, float fs_hz, float f_hz, float damping,
                     uint32_t window, uint8_t in_shift);

/* Clear the state (resonator, low-pass, window counter), keep the
 * coefficients. */
void goertzel_i_reset(goertzel_i_t *g);

/* Run n samples; write every window-th low-passed magnitude (units of
 * the shifted input sample) to mag_out. Returns the count written. */
uint32_t goertzel_i_block(goertzel_i_t *g, const uint16_t *x, uint32_t n,
                          int32_t *mag_out);

#endif /* GOERTZEL_I_H */

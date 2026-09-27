/*
 * iir1.h - first-order IIR low-pass / high-pass with a shift instead of a
 *          multiply, one instance per filter (iir1.c)
 *
 * Taken over on 27.09.2026 (P3.2) from the template
 * Goertzel/goertzel/firmware/src/goertzel.c, iFLT_IIR1_Lowpass() (lines
 * 169-207) and iFLT_IIR1_Highpass() (lines 210-234). The arithmetic is
 * the template's, unchanged:
 *
 *     tap = tap - (tap >> k) + x        (both)
 *     LP:  y = tap >> k                 H(z) = 2^-k / (1 - (1 - 2^-k) z^-1)
 *     HP:  y = x - (tap >> k)           H(z) = (1 - z^-1) / (1 - (1 - 2^-k) z^-1)
 *
 * What changed: the template keeps the taps in one global array
 * iIIR_Tap[N_FILTER], indexed by an int, and FLT_vIIR_Init() clears ALL
 * of them - so a detection on one channel reset the low-pass of every
 * other (DESIGN-MULTICHANNEL.md 4.3). Here every filter is an iir1_t the
 * caller owns, and k (the template's LP_SHIFT_VALUE_K / HP_SHIFT_VALUE_K,
 * both 4) is a per-instance parameter. No static state anywhere.
 *
 * The tap holds 2^k times the low-pass output (DC gain of the tap is
 * 2^k). It is an int32_t, so the input must satisfy |x| <= 2^(31-k) - 1;
 * the first-order response has no overshoot, so the tap then stays
 * within +-(2^31 - 2^k) - tested at that limit for k = 4 in
 * tests/host/test_iir1.c. k must be 1..30.
 *
 * `>>` on a negative int32_t is an arithmetic shift (rounds towards
 * minus infinity) with xc-dsc and with gcc; the template relies on that
 * and so does this, and the test vectors (tests/ref/goertzel_ref.py, whose
 * Python ints shift the same way) pin it down with negative inputs.
 *
 * Rise time and bandwidth per k, from the template's comment
 * (normalised to fs = 1 Hz):
 *   k  bandwidth  rise (samples)     k  bandwidth  rise
 *   1  0.1197       3                5  0.0051      69
 *   2  0.0466       8                6  0.0026     140
 *   3  0.0217      16                7  0.0012     280
 *   4  0.0104      34                8  0.0007     561
 */
#ifndef IIR1_H
#define IIR1_H

#include <stdint.h>

typedef struct {
    int32_t tap;    /* the one state: 2^k x the low-pass output */
    uint8_t k;      /* the shift, 1..30 */
} iir1_t;

/* Set k and clear the state. */
void iir1_init(iir1_t *f, uint8_t k);

/* Clear the state, keep k (the template's FLT_vIIR_Init(), per instance). */
void iir1_reset(iir1_t *f);

/* One low-pass step: returns tap >> k after the update. */
int32_t iir1_lp(iir1_t *f, int32_t x);

/* One high-pass step: returns x - (tap >> k) after the update. */
int32_t iir1_hp(iir1_t *f, int32_t x);

#endif /* IIR1_H */

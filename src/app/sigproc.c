/*
 * sigproc.c - the body of the signal processing, called once per completed
 * half of the ping-pong buffer (ping and pong) while "sigproc on" is set.
 * See sigproc.h for when it is called, how much time it has, what it must
 * not do, and why the result goes back into the same half.
 *
 * Since 02.10.2026: a 4th-order Butterworth low-pass whose cut-off lies
 * exactly in the middle of the useful band - the band runs from 0 to the
 * Nyquist frequency fs/2, so the cut-off is fs/4. Being a fraction of fs,
 * the coefficients do not depend on the sample rate: the filter follows
 * "stream on <ksps>" by itself.
 *
 * Design (bilinear transform, pre-warped): the analogue 4th-order
 * Butterworth prototype is two sections 1 / (s^2 + c s + 1) with
 * c1 = 2 sin(pi/8) and c2 = 2 sin(3 pi/8). At fc = fs/4 the pre-warp
 * factor is tan(pi fc / fs) = tan(pi/4) = 1, and each section becomes
 *
 *     H(z) = g (1 + 2 z^-1 + z^-2) / (1 + a2 z^-2),
 *     g = 1 / (2 + c),  a2 = (2 - c) / (2 + c)
 *
 * - a1 is exactly 0 and each section's gain at DC is exactly 1. Checked
 * numerically (02.10.2026): 0 dB at DC, -0.32 dB at 0.2 fs, -3.01 dB at
 * 0.25 fs, -11.4 dB at 0.3 fs, -30.6 dB at 0.375 fs, a double zero at fs/2.
 *
 * Form: transposed direct form II per section, in float on the FPU, the
 * state carried from one block to the next - the ping-pong stream has no
 * gaps (two pairs, dma.c), so neither does the filter's input. Where the
 * capture says there is one (info->gap: the first block after "sigproc
 * on" or a stream start, or halves missed), the state is set to the
 * steady state of the block's first sample, so the filter starts without
 * a transient instead of filtering the jump. Output rounded and clamped
 * to the ADC's 0..4095 (the step response overshoots by about 10 %), and
 * written back in place.
 */
#include "sigproc.h"

#define LP_G1   0.361615673f    /* section 1: c = 2 sin(pi/8) = 0.765367   */
#define LP_A1   0.446462692f
#define LP_G2   0.259891532f    /* section 2: c = 2 sin(3pi/8) = 1.847759  */
#define LP_A2   0.039566130f
#define LP_G    (LP_G1 * LP_G2) /* both sections' gains, applied once        */

/* Each section runs WITHOUT its gain g, and the product g1 g2 multiplies
 * the cascade's output once - the same filter, fewer multiplies. With
 * a1 = 0 a section's output depends on its own output two samples back
 * only:
 *     y[n] = x[n] + 2 x[n-1] + x[n-2] - a2 y[n-2]
 * so the even and the odd samples form two independent recursions, and
 * the loop does two samples per pass: the FPU can work on both chains at
 * once instead of waiting for each result before the next operation.
 * Measured on the board (02.10.2026, "free CPU cycles per sample"): 59
 * cycles per sample with the state in statics and one sample per pass,
 * 43 with the state in locals; see HARDWARE-LOG for this version. The
 * state lives in locals for the block and goes back to `lp` at its end.
 * Unscaled, the values grow by 1/g1 = 2.77 and 1/(g1 g2) = 10.6 -
 * nothing for a float. */
typedef struct {
    float x1, x2;       /* input, one and two samples back                  */
    float a1, a2;       /* section 1 output, one and two samples back       */
    float b1, b2;       /* section 2 output, one and two samples back       */
} lp_state_t;
static lp_state_t lp;

/* The state after a long constant input x: section 1's unscaled DC gain
 * is 4 / (1 + a2) = 1 / g1, section 2's likewise. */
static void lp_settle(float x)
{
    const float y1 = x / LP_G1;
    const float y2 = y1 / LP_G2;
    lp.x1 = x;  lp.x2 = x;
    lp.a1 = y1; lp.a2 = y1;
    lp.b1 = y2; lp.b2 = y2;
}

static inline uint16_t lp_out(float y2)
{
    int32_t v = (int32_t)(LP_G * y2 + 0.5f);
    if (v < 0)    { v = 0; }
    if (v > 4095) { v = 4095; }
    return (uint16_t)v;
}

void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    if (n == 0u) { return; }
    if (info->gap) { lp_settle((float)x[0]); }
    float x1 = lp.x1, x2 = lp.x2, a1 = lp.a1, a2 = lp.a2, b1 = lp.b1, b2 = lp.b2;
    uint32_t i = 0;
    for (; i + 2u <= n; i += 2u) {
        const float u0 = (float)x[i];
        const float u1 = (float)x[i + 1u];
        /* section 1: sample i from a2 (i-2), sample i+1 from a1 (i-1) */
        const float y0 = (u0 + x2) + (x1 + x1) - LP_A1 * a2;
        const float y1 = (u1 + x1) + (u0 + u0) - LP_A1 * a1;
        /* section 2, the same on section 1's output */
        const float v0 = (y0 + a2) + (a1 + a1) - LP_A2 * b2;
        const float v1 = (y1 + a1) + (y0 + y0) - LP_A2 * b1;
        x[i]      = lp_out(v0);
        x[i + 1u] = lp_out(v1);
        x2 = u0; x1 = u1;
        a2 = y0; a1 = y1;
        b2 = v0; b1 = v1;
    }
    for (; i < n; i++) {                            /* an odd tail (n is even) */
        const float u = (float)x[i];
        const float y = (u + x2) + (x1 + x1) - LP_A1 * a2;
        const float v = (y + a2) + (a1 + a1) - LP_A2 * b2;
        x[i] = lp_out(v);
        x2 = x1; x1 = u;
        a2 = a1; a1 = y;
        b2 = b1; b1 = v;
    }
    lp.x1 = x1; lp.x2 = x2; lp.a1 = a1; lp.a2 = a2; lp.b1 = b1; lp.b2 = b2;
}

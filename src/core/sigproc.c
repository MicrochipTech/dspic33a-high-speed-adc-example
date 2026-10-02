/*
 * sigproc.c - the body of the signal processing, called once per completed
 * half of the ping-pong buffer (ping and pong) while "sigproc on" is set.
 * See sigproc.h for when it is called, how much time it has, what it must
 * not do, and why the result goes back into the same half.
 *
 * A 4th-order Butterworth low-pass with its cut-off at fs/8 - a quarter of
 * the useful band 0..fs/2 (fs/4, the middle of the band, from 02.10.2026
 * until it was halved the same day). Being a fraction of fs, the
 * coefficients do not depend on the sample rate: the filter follows
 * "stream on <ksps>" by itself.
 *
 * Design (bilinear transform, pre-warped): the analogue 4th-order
 * Butterworth prototype is two sections 1 / (s^2 + c s + 1) with
 * c1 = 2 sin(pi/8) and c2 = 2 sin(3 pi/8). With the pre-warp factor
 * K = tan(pi fc / fs) = tan(pi/8) = 0.414214 each section becomes
 *
 *     H(z) = g (1 + 2 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2),
 *     a0 = 1 + c K + K^2,  g = K^2 / a0,
 *     a1 = 2 (K^2 - 1) / a0,  a2 = (1 - c K + K^2) / a0
 *
 * - each section's gain at DC is exactly 1. Checked numerically
 * (02.10.2026): 0 dB at DC, -0.58 dB at 0.1 fs, -3.01 dB at 0.125 fs,
 * -7.95 dB at 0.15 fs, -19.6 dB at 0.2 fs, -30.6 dB at 0.25 fs, -41.7 dB at
 * 0.3 fs, a double zero at fs/2. (At fs/4, K was 1 and a1 0; with a1 not
 * 0 the even and odd samples are no longer independent recursions, so
 * the loop does one sample per pass again.)
 *
 * The state is carried from one block to the next - the ping-pong stream
 * has no gaps (two pairs, dma.c), so neither does the filter's input.
 * Where the capture says there is one (info->gap: the first block after
 * "sigproc on" or a stream start, or halves missed), the state is set to
 * the steady state of the block's first sample, so the filter starts
 * without a transient instead of filtering the jump. Output rounded and
 * clamped to the ADC's 0..4095 (the step response overshoots by about
 * 10 %), and written back in place.
 */
#include "sigproc.h"

#define LP_G1   0.115258015f    /* section 1: c = 2 sin(pi/8) = 0.765367   */
#define LP_C1  -1.113029854f    /*            a1                           */
#define LP_D1   0.574061915f    /*            a2                           */
#define LP_G2   0.088579356f    /* section 2: c = 2 sin(3pi/8) = 1.847759  */
#define LP_C2  -0.855397933f    /*            a1                           */
#define LP_D2   0.209715358f    /*            a2                           */
#define LP_G    (LP_G1 * LP_G2) /* both sections' gains, applied once      */

/* Each section runs WITHOUT its gain g, as a direct form I:
 *     y[n] = x[n] + 2 x[n-1] + x[n-2] - a1 y[n-1] - a2 y[n-2]
 * and the product g1 g2 multiplies the cascade's output once - the same
 * filter, one multiply less per section. The state lives in locals for the
 * block and goes back to `lp` at its end (kept in statics per sample it
 * cost a load and a store each time - 59 instead of 43 CPU cycles per
 * sample at fs/4, board, 02.10.2026). Unscaled, the values grow by
 * 1/g1 = 8.7 and 1/(g1 g2) = 98 - for a float that is a rounding error
 * below a thousandth of an LSB at the output. */
typedef struct {
    float x1, x2;       /* input, one and two samples back                  */
    float p1, p2;       /* section 1 output, one and two samples back       */
    float q1, q2;       /* section 2 output, one and two samples back       */
} lp_state_t;
static lp_state_t lp;

/* The state after a long constant input x: each unscaled section's DC
 * gain is 4 / (1 + a1 + a2) = 1 / g. */
static void lp_settle(float x)
{
    const float p = x / LP_G1;
    const float q = p / LP_G2;
    lp.x1 = x;  lp.x2 = x;
    lp.p1 = p;  lp.p2 = p;
    lp.q1 = q;  lp.q2 = q;
}

void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    if (n == 0u) { return; }
    if (info->gap) { lp_settle((float)x[0]); }
    float x1 = lp.x1, x2 = lp.x2, p1 = lp.p1, p2 = lp.p2, q1 = lp.q1, q2 = lp.q2;
    for (uint32_t i = 0; i < n; i++) {
        const float u = (float)x[i];
        const float p = (u + x2) + (x1 + x1) - LP_C1 * p1 - LP_D1 * p2;   /* section 1 */
        const float q = (p + p2) + (p1 + p1) - LP_C2 * q1 - LP_D2 * q2;   /* section 2 */
        int32_t v = (int32_t)(LP_G * q + 0.5f);
        if (v < 0)    { v = 0; }
        if (v > 4095) { v = 4095; }
        x[i] = (uint16_t)v;
        x2 = x1; x1 = u;
        p2 = p1; p1 = p;
        q2 = q1; q1 = q;
    }
    lp.x1 = x1; lp.x2 = x2; lp.p1 = p1; lp.p2 = p2; lp.q1 = q1; lp.q2 = q2;
}

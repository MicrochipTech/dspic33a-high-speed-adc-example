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
 * sigproc.c - the body of the signal processing, called once per completed
 * half of the ping-pong buffer (ping and pong) while the processing is on.
 * See sigproc.h for when it is called, how much time it has, what it must
 * not do, and why the result goes back into the same half.
 *
 * Selectable since 02.10.2026 ("sigproc lp|hp|bp|off", the GUI's signal
 * processing card): one of three 4th-order Butterworth filters at fs/8, and
 * independently of it a Goertzel detector for a tone at fs/16. Being
 * fractions of fs, none of the coefficients depends on the sample rate: they
 * follow "stream on <ksps>" by themselves.
 *
 *   lp   low-pass,  -3 dB at fs/8                  (the filter of the
 *        morning of 02.10.2026, fs/4 before that, numbers unchanged)
 *   hp   high-pass, -3 dB at fs/8
 *   bp   band-pass, centre fs/8, one octave: -3 dB at fs/8/sqrt2, fs/8*sqrt2
 *
 * Design: tools/sigproc_design.py (bilinear transform, pre-warped, checked
 * there against the analog Butterworth magnitude to 1e-6). Each filter is
 * two biquad sections g (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
 * with the numerator fixed per kind - (1, 2, 1) low-pass, (1, -2, 1)
 * high-pass, (1, 0, -1) band-pass - and the two gains applied once to the
 * cascade's output. |H| from the script:
 *
 *   f/fs   0.02    0.0625  0.0884  0.10    0.125   0.15    0.1768  0.20    0.25
 *   lp     1.0000  0.9986  0.9758  0.9352  0.7071  0.4002  0.1948  0.1051  0.0294
 *   hp     0.0005  0.0531  0.2188  0.3541  0.7071  0.9164  0.9808  0.9945  0.9996
 *   bp     0.0149  0.2299  0.7071  0.9194  1.0000  0.9736  0.7071  0.4343  0.1638
 *
 * The high- and band-pass block DC, so their output is centred on mid-scale:
 * result + 2048, clamped to 0..4095 like the low-pass's - the GUI reads
 * 12-bit values (sigproc.h).
 *
 * The state is carried from one block to the next - the ping-pong stream
 * has no gaps (two pairs, dma.c), so neither does the filter's input. On a
 * gap (info->gap) or a change of filter it restarts as if the block's first
 * sample had been there forever: the low-pass then outputs it unchanged,
 * high- and band-pass output mid-scale.
 *
 * The Goertzel (when on) runs on the block BEFORE the filter - it detects a
 * tone at fs/16 in the input, whatever the filter then does to it. What it
 * computes is the one DFT value a Goertzel gives,
 *     X = sum over i of (x[i] - mean) e^(-j 2 pi i / 16),
 * and from it the tone's amplitude A = 2 |X| / n (LSB) - exact for a sine at
 * fs/16 when n is a multiple of 16 (buf's default 1024 is; another n leaks a
 * little) - the signal's rms around its mean, the tone's share of the
 * signal's power (A^2/2 over the variance, per mille), and "detected" =
 * A >= the threshold (default 100 LSB, "sigproc gz thr"). Taking the mean
 * out keeps a 2000-LSB DC level from leaking into the bin when n is not a
 * multiple of 16.
 *
 * How (optimised the same day): because e^(-j 2 pi i / 16) repeats every 16
 * samples, the block is first FOLDED - one pass of integer adds, sample i
 * into acc[i mod 16], with the sum of squares (64 bit) alongside - and X is
 * then 16 complex multiplies once per block:
 *     X = sum over k of (acc[k] - mean cnt[k]) e^(-j 2 pi k / 16),
 * cnt[k] being how many samples fell into acc[k], so the mean comes out
 * exactly for any n. The variance is exact too, from integers:
 * (n sumsq - sum^2) / n^2. The first version - two passes, the float
 * recursion s = d + 2 cos(2 pi/16) s1 - s2 per sample, its dependency chain
 * and an int-to-float conversion each time - cost up to 55 CPU cycles per
 * sample (27 % of the budget at 1 MSPS, board, 02.10.2026); folded it costs
 * about 7 (board, same day: 33 per mille at 1 MSPS, 132 at 4, 264 at
 * 8 MSPS with nothing missed). The result is the same number;
 * tests/host/test_sigproc.c holds it to a double-precision DFT.
 *
 * A project's own analysis of the blocks hooks in through
 * sigproc_app_block() and its companions (sigproc.h), defined in a file of
 * its own; the weak defaults at the end of this file do nothing.
 */
#include <stdbool.h>
#include <math.h>
#include "sigproc.h"

/* tools/sigproc_design.py */
#define LP_G1   0.115258015f
#define LP_C1  -1.113029854f
#define LP_D1   0.574061915f
#define LP_G2   0.088579356f
#define LP_C2  -0.855397933f
#define LP_D2   0.209715358f
#define HP_G1   0.671772942f
#define HP_C1  -1.113029854f
#define HP_D1   0.574061915f
#define HP_G2   0.516278323f
#define HP_C2  -0.855397933f
#define HP_D2   0.209715358f
#define BP_G1   0.264190631f
#define BP_C1  -1.418769755f
#define BP_D1   0.731414325f
#define BP_G2   0.207189199f
#define BP_C2  -0.845887980f
#define BP_D2   0.624616423f

#define GZ_THR_DEFAULT 100u           /* LSB                             */
#define MID            2048.0f        /* hp/bp output offset             */

/* Each section runs WITHOUT its gain g, as a direct form I:
 *     y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]
 * and the product g1 g2 multiplies the cascade's output once - the same
 * filter, one multiply less per section. The state lives in locals for the
 * block and goes back to `st` at its end (kept in statics per sample it
 * cost a load and a store each time - 59 instead of 43 CPU cycles per
 * sample at fs/4, board, 02.10.2026). Unscaled, the values grow by up to
 * 1/(g1 g2) (98 for the low-pass) - for a float a rounding error below a
 * thousandth of an LSB at the output. */
typedef struct {
    float x1, x2;       /* input, one and two samples back                  */
    float p1, p2;       /* section 1 output, one and two samples back       */
    float q1, q2;       /* section 2 output, one and two samples back       */
} fstate_t;
static fstate_t st;

static volatile sigproc_filter_t filter = SIGPROC_OFF;  /* off after reset; "sigproc on" = lp */
static volatile bool restart = true;    /* a new filter: settle on the next block */
static volatile bool gz_on   = false;
static volatile uint32_t gz_thr = GZ_THR_DEFAULT;
static sigproc_gz_t gz;                 /* the last block's result         */

/* The state after a long constant input u: each unscaled section's gain at
 * DC is b(1) / (1 + a1 + a2) - 1/g for the low-pass, 0 for the others. */
static void settle(sigproc_filter_t f, float u)
{
    st.x1 = u;  st.x2 = u;
    if (f == SIGPROC_LP) {
        st.p1 = st.p2 = u / LP_G1;
        st.q1 = st.q2 = u / LP_G1 / LP_G2;
    } else {
        st.p1 = st.p2 = 0.0f;
        st.q1 = st.q2 = 0.0f;
    }
}

/* The cascade, specialised per filter: always inlined with constant
 * arguments, so the numerator's 2 / -2 / 0 and the offset fold away and the
 * low-pass loop is the one it was before the other two came. */
static inline __attribute__((always_inline))
void cascade(uint16_t *x, uint32_t n, float b1, float b2,
             float c1, float d1, float c2, float d2, float g, float offset)
{
    float x1 = st.x1, x2 = st.x2, p1 = st.p1, p2 = st.p2, q1 = st.q1, q2 = st.q2;
    for (uint32_t i = 0; i < n; i++) {
        const float u = (float)x[i];
        const float p = (u + b2 * x2) + b1 * x1 - c1 * p1 - d1 * p2;   /* section 1 */
        const float q = (p + b2 * p2) + b1 * p1 - c2 * q1 - d2 * q2;   /* section 2 */
        int32_t v = (int32_t)(g * q + offset + 0.5f);
        if (v < 0)    { v = 0; }
        if (v > 4095) { v = 4095; }
        x[i] = (uint16_t)v;
        x2 = x1; x1 = u;
        p2 = p1; p1 = p;
        q2 = q1; q1 = q;
    }
    st.x1 = x1; st.x2 = x2; st.p1 = p1; st.p2 = p2; st.q1 = q1; st.q2 = q2;
}

/* cos and sin of 2 pi k / 16, k = 0..15 */
static const float gz_cos[16] = {
     1.000000000f,  0.923879533f,  0.707106781f,  0.382683432f,
     0.000000000f, -0.382683432f, -0.707106781f, -0.923879533f,
    -1.000000000f, -0.923879533f, -0.707106781f, -0.382683432f,
     0.000000000f,  0.382683432f,  0.707106781f,  0.923879533f };
static const float gz_sin[16] = {
     0.000000000f,  0.382683432f,  0.707106781f,  0.923879533f,
     1.000000000f,  0.923879533f,  0.707106781f,  0.382683432f,
     0.000000000f, -0.382683432f, -0.707106781f, -0.923879533f,
    -1.000000000f, -0.923879533f, -0.707106781f, -0.382683432f };

/* One step of the fold: sample i + K into acc[K], its square into the sum. */
#define GZ_FOLD(K) do { const uint32_t v_ = x[i + (K)]; acc[K] += v_; sq += v_ * v_; } while (0)

static void goertzel(const uint16_t *x, uint32_t n, uint32_t seq)
{
    /* The fold. Bounds: n <= 1024 (SAMPLES_PER_HALF_MAX), so acc[k] <= 64 x
     * 4095 and the sum fit 32 bits easily; one square is at most 4095^2 <
     * 2^24, and sq is summed in 32 bits per 16 samples (16 x 2^24 = 2^28)
     * before it goes into the 64-bit total - one 64-bit add per 16 samples
     * instead of per sample. */
    uint32_t acc[16] = { 0u };
    uint64_t sumsq = 0u;
    uint32_t i = 0u;
    for (; i + 16u <= n; i += 16u) {
        uint32_t sq = 0u;
        GZ_FOLD(0);  GZ_FOLD(1);  GZ_FOLD(2);  GZ_FOLD(3);
        GZ_FOLD(4);  GZ_FOLD(5);  GZ_FOLD(6);  GZ_FOLD(7);
        GZ_FOLD(8);  GZ_FOLD(9);  GZ_FOLD(10); GZ_FOLD(11);
        GZ_FOLD(12); GZ_FOLD(13); GZ_FOLD(14); GZ_FOLD(15);
        sumsq += sq;
    }
    {
        uint32_t sq = 0u;
        for (uint32_t k = 0u; i < n; i++, k++) {      /* the last n mod 16 */
            const uint32_t v = x[i];
            acc[k] += v;
            sq += v * v;
        }
        sumsq += sq;
    }

    /* Once per block: the mean, the DFT value from the 16 sums, the variance. */
    uint32_t sum = 0u;
    for (uint32_t k = 0u; k < 16u; k++) { sum += acc[k]; }
    const float mean = (float)sum / (float)n;
    const uint32_t full = n / 16u, rem = n % 16u;
    float xr = 0.0f, xi = 0.0f;
    for (uint32_t k = 0u; k < 16u; k++) {
        const float a = (float)acc[k] - mean * (float)(full + ((k < rem) ? 1u : 0u));
        xr += a * gz_cos[k];
        xi -= a * gz_sin[k];
    }
    const float amp = 2.0f * sqrtf(xr * xr + xi * xi) / (float)n;
    const uint64_t nv = (uint64_t)n * sumsq - (uint64_t)sum * sum;   /* n^2 variance, >= 0 */
    const float var = (float)nv / ((float)n * (float)n);
    const float share = (var > 0.0f) ? (amp * amp * 0.5f / var) : 0.0f;
    gz.amp       = (uint32_t)(amp + 0.5f);
    gz.rms       = (uint32_t)(sqrtf(var) + 0.5f);
    gz.share_pm  = (share >= 1.0f) ? 1000u : (uint32_t)(share * 1000.0f + 0.5f);
    gz.thr       = gz_thr;
    gz.detected  = (gz.amp >= gz_thr) ? 1u : 0u;
    gz.seq       = seq;
    gz.valid     = 1u;
}

void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    if (n == 0u) { return; }
    if (gz_on) { goertzel(x, n, info->seq); }
    sigproc_app_block(x, n, info);
    const sigproc_filter_t f = filter;          /* one reading per block */
    if (f == SIGPROC_OFF) { return; }
    if (info->gap || restart) { settle(f, (float)x[0]); restart = false; }
    switch (f) {
    case SIGPROC_LP:
        cascade(x, n, 2.0f, 1.0f, LP_C1, LP_D1, LP_C2, LP_D2, LP_G1 * LP_G2, 0.0f);
        break;
    case SIGPROC_HP:
        cascade(x, n, -2.0f, 1.0f, HP_C1, HP_D1, HP_C2, HP_D2, HP_G1 * HP_G2, MID);
        break;
    case SIGPROC_BP:
        cascade(x, n, 0.0f, -1.0f, BP_C1, BP_D1, BP_C2, BP_D2, BP_G1 * BP_G2, MID);
        break;
    default:
        break;
    }
}

void sigproc_set_filter(sigproc_filter_t f)
{
    if (f > SIGPROC_BP) { return; }
    if (f != filter) { restart = true; }
    filter = f;
}

sigproc_filter_t sigproc_filter(void) { return filter; }

const char *sigproc_filter_name(sigproc_filter_t f)
{
    static const char *const names[] = { "off", "lp", "hp", "bp" };
    return (f <= SIGPROC_BP) ? names[f] : "?";
}

void sigproc_set_goertzel(bool on)
{
    if (on && !gz_on) { gz.valid = 0u; }        /* no stale result from before */
    gz_on = on;
}

bool sigproc_goertzel_on(void) { return gz_on; }

void sigproc_set_threshold(uint32_t lsb) { gz_thr = lsb; }

void sigproc_goertzel_get(sigproc_gz_t *out)
{
    *out = gz;
    out->thr = gz_thr;
}

void sigproc_set_fs(float fs_hz) { sigproc_app_set_fs(fs_hz); }

/* The application's processing (sigproc.h): nothing unless a project
 * defines these itself. */
__attribute__((weak)) bool sigproc_app_active(void) { return false; }
__attribute__((weak)) void sigproc_app_set_fs(float fs_hz) { (void)fs_hz; }
__attribute__((weak)) void sigproc_app_block(const uint16_t *x, uint32_t n,
                                             const sigproc_info_t *info)
{
    (void)x; (void)n; (void)info;
}

bool sigproc_active(void)
{
    return (filter != SIGPROC_OFF) || gz_on || sigproc_app_active();
}

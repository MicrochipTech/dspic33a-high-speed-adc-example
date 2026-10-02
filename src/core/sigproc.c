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
 * The impact counter (CNT, 02.10.2026, docs/IMPLEMENTATION-PLAN.md): balls
 * falling onto a metal plate, each impact a ring at f (20..200 kHz) that
 * dies away within 0.5 ms, up to about 1000 a second. Per sample, on the
 * input before the filter:
 *     d  = x[i] - x[i-1]                      (the first difference)
 *     q0 = d + 2 D cos(w) q1 - D^2 q2         (a damped Goertzel resonator)
 * with w = 2 pi f / fs and D = exp(-1 / (tau fs)), and every 4th sample the
 * magnitude |q1 - e^(-jw) q2| against the threshold: count once above thr,
 * re-arm below thr / 2. Two departures from lib/goertzel_f, which the plan
 * had named: (1) the first difference - the raw samples sit at about 2000
 * LSB, and that DC level puts a standing magnitude into the resonator of
 * the same order as a ring's (about 6500 for 50 kHz, tau 25 us, 1 MSPS);
 * the difference removes DC exactly, with no state of its own. (2) the
 * magnitude only every 4th sample, squared, against squared thresholds -
 * no square root, no low-pass, no per-sample magnitude buffer. The
 * detector is lib/detect's rule (fire once, re-arm by hysteresis 0.5),
 * written out here on the squared magnitude. The magnitude is scaled so a
 * steady tone of amplitude A at f reads A (LSB): cnt_setup() computes the
 * chain's gain once per configuration (exactly, see there), so the
 * threshold is in the units of the signal, whatever f, tau and fs are.
 *
 * tau: best set to the ring's own decay time - the resonator is then the
 * matched filter for a damped sine. Default 100 us. What it can and cannot
 * do (numbers for a 1000-LSB ring at 50 kHz decaying with 100 us, 1 MSPS,
 * computed 02.10.2026 before building): its peak magnitude is 368 at tau
 * 100 (629 at 25, 250 at 200 - a short ring never reaches a steady tone's
 * full amplitude); it falls below a quarter of that 270 us after the peak,
 * so impacts 1 ms apart are counted one by one and ones closer than about
 * 0.3 ms merge. A ring at ANOTHER frequency still moves it - a short burst
 * is broadband: 19 % of the on-frequency peak at 25 kHz, 49 % at 40, 64 %
 * at 60, 39 % at 80, 29 % at 100 kHz. Telling the frequencies apart is
 * therefore a matter of the threshold sitting between those levels, about
 * 2.5:1 here, not an absolute property.
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

/* The impact counter (see the header). Configuration from the console,
 * applied at the next block (cnt_dirty): the console never runs while a
 * block is processed (cli.c's uart_rx_hook()). */
#define CNT_EVERY 4u                    /* the magnitude every 4th sample  */
static struct {
    volatile bool on, dirty, reset;
    volatile uint32_t f_hz, tau_us, thr;
    volatile float fs;
    float c, dd, cw, sw;                /* 2 D cos w, D^2, cos w, sin w     */
    float thr2, rearm2, inv_scale2;     /* squared, in resonator units     */
    float q1, q2, xp;                   /* resonator, previous sample      */
    bool armed, ready;
    uint32_t count, missed0, missed;
    uint64_t seen;                      /* samples processed since reset   */
    float peak2;                        /* squared, resonator units        */
} cn = { .f_hz = 50000u, .tau_us = 100u, .thr = 100u, .armed = true };

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

/* The resonator's coefficients and its gain for a steady tone at f, once
 * per configuration (from sigproc_block(), main loop). Analytic and exact:
 * for q[n] = Q e^(jwn) the magnitude |q1 - e^(-jw) q2| is |Q| |1 - e^(-2jw)|,
 * and for the negative frequency e^(-jwn) it is exactly 0 - the steady
 * magnitude has no ripple. A sine of amplitude 1 is half of each, so
 *     scale = 1/2 |1 - e^(-jw)| |H(e^(jw))| |1 - e^(-2jw)|
 *           = 2 sin(w/2) sin(w) / |1 - c e^(-jw) + D^2 e^(-2jw)|
 * (the first factor is the first difference). */
static void cnt_setup(void)
{
    cn.ready = false;
    const float fs = cn.fs;
    if (fs <= 0.0f) { return; }
    const float w = 6.283185307f * (float)cn.f_hz / fs;
    const float d = expf(-1.0e6f / ((float)cn.tau_us * fs));
    cn.cw = cosf(w);
    cn.sw = sinf(w);
    cn.c  = 2.0f * d * cn.cw;
    cn.dd = d * d;
    const float c2w = cosf(2.0f * w), s2w = sinf(2.0f * w);
    const float dre = 1.0f - cn.c * cn.cw + cn.dd * c2w;
    const float dim = cn.c * cn.sw - cn.dd * s2w;
    const float scale = 2.0f * sinf(0.5f * w) * cn.sw / sqrtf(dre * dre + dim * dim);
    const float t = (float)cn.thr * scale;
    cn.thr2 = t * t;
    cn.rearm2 = 0.25f * cn.thr2;                   /* (thr / 2)^2 */
    cn.inv_scale2 = 1.0f / (scale * scale);
    cn.q1 = cn.q2 = 0.0f;
    cn.ready = true;
}

static void cnt_block(const uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    if (cn.dirty) { cn.dirty = false; cnt_setup(); }
    if (cn.reset) {
        cn.reset = false;
        cn.count = 0u; cn.seen = 0u; cn.peak2 = 0.0f;
        cn.missed0 = info->missed;
        /* armed only if nothing is ringing right now: a reset in the
         * middle of a ring must not count that ring as a new one */
        const float re = cn.q1 - cn.q2 * cn.cw, im = cn.q2 * cn.sw;
        cn.armed = !cn.ready || ((re * re + im * im) < cn.rearm2);
    }
    if (!cn.ready) { return; }
    if (info->gap) { cn.q1 = cn.q2 = 0.0f; cn.xp = (float)x[0]; }   /* no step from before */
    cn.missed = info->missed - cn.missed0;
    float q1 = cn.q1, q2 = cn.q2, xp = cn.xp, pk = cn.peak2;
    const float c = cn.c, dd = cn.dd, cw = cn.cw, sw = cn.sw;
    const float thr2 = cn.thr2, rearm2 = cn.rearm2;
    bool armed = cn.armed;
    uint32_t count = cn.count, i = 0u;
    for (; i + CNT_EVERY <= n; i += CNT_EVERY) {
        for (uint32_t k = 0; k < CNT_EVERY; k++) {
            const float u = (float)x[i + k];
            const float q0 = (u - xp) + c * q1 - dd * q2;
            xp = u; q2 = q1; q1 = q0;
        }
        const float re = q1 - q2 * cw, im = q2 * sw;
        const float m2 = re * re + im * im;
        if (m2 > pk) { pk = m2; }
        if (armed) {
            if (m2 > thr2) { count++; armed = false; }
        } else if (m2 < rearm2) {
            armed = true;
        }
    }
    for (; i < n; i++) {                              /* n mod 4: no evaluation */
        const float u = (float)x[i];
        const float q0 = (u - xp) + c * q1 - dd * q2;
        xp = u; q2 = q1; q1 = q0;
    }
    cn.q1 = q1; cn.q2 = q2; cn.xp = xp; cn.peak2 = pk;
    cn.armed = armed; cn.count = count;
    cn.seen += n;
}

void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    if (n == 0u) { return; }
    if (gz_on) { goertzel(x, n, info->seq); }
    if (cn.on) { cnt_block(x, n, info); }
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

void sigproc_set_fs(float fs_hz)
{
    if (fs_hz != cn.fs) { cn.fs = fs_hz; cn.dirty = true; }
}

void sigproc_cnt_enable(bool on)
{
    if (on && !cn.on) { cn.dirty = true; cn.reset = true; }
    cn.on = on;
}

bool sigproc_cnt_on(void) { return cn.on; }

bool sigproc_cnt_config(uint32_t f_hz, uint32_t tau_us, uint32_t thr)
{
    if ((f_hz < 1000u) || (tau_us < 2u) || (tau_us > 5000u) || (thr < 1u) || (thr > 4095u)) {
        return false;
    }
    if ((cn.fs > 0.0f) && ((float)f_hz * 2.5f > cn.fs)) { return false; }
    cn.f_hz = f_hz; cn.tau_us = tau_us; cn.thr = thr;
    cn.dirty = true;
    return true;
}

void sigproc_cnt_reset(void) { cn.reset = true; }

void sigproc_cnt_get(sigproc_cnt_t *out, bool clear_peak)
{
    out->on = cn.on ? 1u : 0u;
    out->f_hz = cn.f_hz;
    out->tau_us = cn.tau_us;
    out->thr = cn.thr;
    out->fs_hz = (uint32_t)(cn.fs + 0.5f);
    const bool fresh = cn.reset;                  /* a reset not yet applied */
    out->count = fresh ? 0u : cn.count;
    out->missed = fresh ? 0u : cn.missed;
    const float secs = (cn.fs > 0.0f && !fresh) ? (float)cn.seen / cn.fs : 0.0f;
    out->ms = (uint32_t)(secs * 1000.0f + 0.5f);
    out->rate = (secs > 0.0f) ? (uint32_t)((float)out->count / secs + 0.5f) : 0u;
    out->peak = (cn.ready && !fresh) ? (uint32_t)(sqrtf(cn.peak2 * cn.inv_scale2) + 0.5f) : 0u;
    if (clear_peak) { cn.peak2 = 0.0f; }
}

bool sigproc_active(void) { return (filter != SIGPROC_OFF) || gz_on || cn.on; }

/*
 * test_sigproc.c - src/core/sigproc.c's 4th-order Butterworth low-pass at
 * fs/8 (02.10.2026; fs/4 that morning) on a host gcc: the gain at DC, in
 * the pass band, at the cut-off and in the stop band against the design (the numbers in
 * sigproc.c's comment), no seam between blocks, the re-start on a gap, and
 * the clamp to 0..4095.
 */
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "sigproc.h"
#include "check.h"

#define N      2048u
#define MID    2048.0
#define AMP    1000.0
#define PI     3.14159265358979323846

static uint16_t buf[N];

static void sine(uint16_t *b, uint32_t n, double f, uint32_t start)
{
    for (uint32_t i = 0; i < n; i++) {
        b[i] = (uint16_t)lround(MID + AMP * sin(2.0 * PI * f * (double)(start + i) + 0.3));
    }
}

/* Amplitude of b[from..n-1] around MID: sqrt(2) x RMS. */
static double amplitude(const uint16_t *b, uint32_t from, uint32_t n)
{
    double acc = 0.0;
    for (uint32_t i = from; i < n; i++) { const double d = (double)b[i] - MID; acc += d * d; }
    return sqrt(2.0 * acc / (double)(n - from));
}

static void run(uint16_t *b, uint32_t n, bool gap, uint32_t seq)
{
    const sigproc_info_t info = { seq & 1u, seq, 0u, gap ? 1u : 0u };
    sigproc_block(b, n, &info);
}

/* The gain of one sine through the filter, settled (the first 256 left out). */
static double gain_at(double f)
{
    sine(buf, N, f, 0u);
    run(buf, N, true, 1u);
    return amplitude(buf, 256u, N) / AMP;
}

int main(void)
{
    /* ---- DC passes unchanged, from the very first sample (settled state) */
    for (uint32_t i = 0; i < N; i++) { buf[i] = 2000u; }
    run(buf, N, true, 1u);
    {
        bool all = true;
        for (uint32_t i = 0; i < N; i++) { if (buf[i] != 2000u) { all = false; } }
        CHECK(all);
    }

    /* ---- pass band, cut-off, stop band: |H| per sigproc.c's comment ---- */
    const double g05 = gain_at(0.05), g10 = gain_at(0.10), g125 = gain_at(0.125);
    const double g15 = gain_at(0.15), g20 = gain_at(0.20), g25 = gain_at(0.25);
    CHECK(fabs(g05 - 0.9998) < 0.005);           /* 0 dB                       */
    CHECK(fabs(g10 - 0.9352) < 0.005);           /* -0.58 dB                   */
    CHECK(fabs(g125 - 0.70711) < 0.005);         /* -3.01 dB: the cut-off fs/8 */
    CHECK(fabs(g15 - 0.4002) < 0.005);           /* -7.95 dB                   */
    CHECK(fabs(g20 - 0.1051) < 0.003);           /* -19.6 dB                   */
    CHECK(fabs(g25 - 0.0294) < 0.003);           /* -30.6 dB (rounding: 0.5 LSB) */

    /* ---- no seam: two blocks in a row = one block of twice the length ---- */
    {
        static uint16_t one[N], two[N];
        sine(one, N, 0.07, 0u);
        for (uint32_t i = 0; i < N; i++) { two[i] = one[i]; }
        run(one, N, true, 10u);                              /* one block      */
        run(two, N / 2u, true, 20u);                         /* two halves     */
        run(two + N / 2u, N / 2u, false, 21u);
        bool same = true;
        for (uint32_t i = 0; i < N; i++) { if (one[i] != two[i]) { same = false; } }
        CHECK(same);
    }

    /* ---- a gap re-starts the state at the block's first sample: a
     *      constant block after a sine comes out constant ---- */
    sine(buf, N, 0.1, 0u);
    run(buf, N, true, 30u);
    for (uint32_t i = 0; i < N; i++) { buf[i] = 1234u; }
    run(buf, N, true, 40u);                                  /* gap: no carry  */
    {
        bool all = true;
        for (uint32_t i = 0; i < N; i++) { if (buf[i] != 1234u) { all = false; } }
        CHECK(all);
    }

    /* ---- the clamp: a full-scale step overshoots (about 10 %), and is
     *      held at 4095 - never wraps to a small value ---- */
    for (uint32_t i = 0; i < N; i++) { buf[i] = (i < 64u) ? 0u : 4095u; }
    run(buf, N, true, 50u);
    {
        uint32_t mx = 0u, low_after = 0u;
        for (uint32_t i = 0; i < N; i++) {
            if (buf[i] > mx) { mx = buf[i]; }
            if ((i > 80u) && (buf[i] < 3500u)) { low_after++; }
        }
        CHECK_EQ(mx, 4095u);
        CHECK_EQ(low_after, 0u);
        CHECK_EQ(buf[N - 1u], 4095u);
    }

    return check_summary();
}

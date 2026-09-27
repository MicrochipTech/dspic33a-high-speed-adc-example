/*
 * test_goertzel_i.c - host-side test for lib/goertzel_i.c (P3.4)
 *
 * Built by tools\hosttest.bat against the real src/lib/goertzel_i.c and
 * src/lib/iir1.c. Same vectors and structure as test_goertzel_f.c; what
 * differs is the tolerance, which here comes from the fixed-point
 * quantisation, not from float32:
 *
 * TOLERANCE.
 *  (a) The Q16 coefficients are rounded, |error| <= 2^-17 each. The pole
 *      radius is sqrt(D^2), so an error e in the D^2 constant moves the
 *      radius by e / (2 D) <= 2^-18 = 3.8e-6. The gain at resonance is
 *      proportional to 1 / (1 - r), so the magnitude changes relatively by
 *      3.8e-6 / (1 - D) = 7.6e-4 at D = 0.995 (3.8e-4 at D = 0.99). The
 *      error in 2 D cos(w) moves the centre angle by about 2^-17 /
 *      (2 sin(w)) = 4e-6 rad, second order against the half-width 1 - D
 *      of the resonance: negligible. cos/sin in the output stage: 2^-17
 *      relative, negligible.
 *  (b) Each of the two feedback products truncates (floor after >> 16),
 *      < 1 LSB of the Q12 state per sample, i.e. < 2^-12 in units of s;
 *      the resonator amplifies that by its gain, at most ~1 / (1 - D) =
 *      200: < 0.05 LSB of the output. Negligible.
 *  (c) re and im each truncate < 1 LSB of Q12, the magnitude is then
 *      shifted down by 12 and truncated: < 1 LSB in units of s. Against
 *      the reference's int(magnitude) that is a difference of at most
 *      1 LSB per sample, smoothed by the low-pass to < 1 LSB, plus the
 *      one truncation flip the integer low-pass can propagate: 2 LSB.
 *
 *     |c - ref| <= max(ABS_TOL, REL_TOL x |ref|)
 *     ABS_TOL = 3 LSB    (c) with a margin of one
 *     REL_TOL = 2e-3     (a) at D = 0.995 with a margin of about 2.5
 *
 * The test prints the largest deviation seen, absolute and relative
 * (relative only where the absolute band is exceeded), so the real
 * margin is on record. If the measured error exceeds the derivation, the
 * derivation is wrong, not the tolerance too tight: report it, do not
 * widen it (rule for P3.4).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "goertzel_i.h"
#include "check.h"
#include "vectors.h"

#define ABS_TOL 3.0
#define REL_TOL 2e-3
#define NMAX 4096

static double max_abs_dev = 0.0, max_rel_dev = 0.0;

static int load_input(const char *argv0, const char *name, uint16_t *buf)
{
    FILE *f = vectors_open(argv0, name);
    if (f == NULL) { return -1; }
    double v[2];
    int n = 0, r;
    while ((r = vectors_row(f, v, 2)) == 1 && n < NMAX) { buf[n++] = (uint16_t)v[1]; }
    fclose(f);
    return (r == 0) ? n : -1;
}

static int load_vector(const char *argv0, const char *name, const char *needle,
                       int32_t *s, int32_t *lp_emitted, int *n_emitted)
{
    FILE *f = vectors_open(argv0, name);
    if (f == NULL) { return -1; }
    CHECK(strstr(vectors_header, needle) != NULL);
    double v[5];
    int n = 0, e = 0, r;
    while ((r = vectors_row(f, v, 5)) == 1 && n < NMAX) {
        s[n++] = (int32_t)v[0];
        if (v[3] != 0.0) { lp_emitted[e++] = (int32_t)v[2]; }
    }
    fclose(f);
    *n_emitted = e;
    return (r == 0) ? n : -1;
}

static uint32_t run_blocks(goertzel_i_t *g, const uint16_t *x, int n, int blk,
                           int32_t *out)
{
    uint32_t total = 0;
    for (int i = 0; i < n; i += blk) {
        const int len = (n - i < blk) ? (n - i) : blk;
        total += goertzel_i_block(g, &x[i], (uint32_t)len, &out[total]);
    }
    return total;
}

static void compare(const char *what, const int32_t *c, const int32_t *ref, int n)
{
    for (int i = 0; i < n; i++) {
        const double d = fabs((double)c[i] - (double)ref[i]);
        const double rel = (ref[i] != 0) ? d / fabs((double)ref[i]) : 0.0;
        const double allow = (REL_TOL * fabs((double)ref[i]) > ABS_TOL)
                             ? REL_TOL * fabs((double)ref[i]) : ABS_TOL;
        check_total_++;
        if (d > allow) {
            check_failed_++;
            fprintf(stderr, "%s [%d]: c %ld, ref %ld, |d| %.1f > %.2f\n",
                    what, i, (long)c[i], (long)ref[i], d, allow);
        }
        if (d > max_abs_dev) { max_abs_dev = d; }
        if (d > ABS_TOL && rel > max_rel_dev) { max_rel_dev = rel; }
    }
}

static uint16_t in_a[NMAX], in_b[NMAX];
static int32_t ref_s[NMAX], ref_lp[NMAX];
static int32_t out1[NMAX], out2[NMAX], out3[NMAX];

static uint32_t run_case(const char *argv0, const char *input, const char *vector,
                         const char *needle, float f_hz, float damping,
                         uint32_t window, uint16_t *in)
{
    const int n = load_input(argv0, input, in);
    int n_emit = 0;
    const int nv = load_vector(argv0, vector, needle, ref_s, ref_lp, &n_emit);
    CHECK(n > 0);
    CHECK_EQ(nv, n);
    if (n <= 0 || nv != n) { return 0; }

    int s_ok = 1;
    for (int i = 0; i < n; i++) { if (ref_s[i] != (in[i] >> 2)) { s_ok = 0; } }
    CHECK(s_ok);

    goertzel_i_t g;
    goertzel_i_init(&g, 50000.0f, f_hz, damping, window, 2u);
    const uint32_t c1 = run_blocks(&g, in, n, n, out1);          /* one block */
    CHECK_EQ(c1, (uint32_t)n_emit);
    CHECK_EQ(c1, ((uint32_t)n + window - 1u) / window);
    compare(vector, out1, ref_lp, (int)c1);

    goertzel_i_reset(&g);                                        /* after reset */
    const uint32_t c2 = run_blocks(&g, in, n, n, out2);
    CHECK_EQ(c2, c1);
    CHECK(memcmp(out1, out2, c1 * sizeof out1[0]) == 0);

    goertzel_i_init(&g, 50000.0f, f_hz, damping, window, 2u);   /* blocks of 100 */
    const uint32_t c3 = run_blocks(&g, in, n, 100, out3);
    CHECK_EQ(c3, c1);
    CHECK(memcmp(out1, out3, c1 * sizeof out1[0]) == 0);
    return c1;
}

int main(int argc, char **argv)
{
    const char *argv0 = argc > 0 ? argv[0] : NULL;

    /* ---- coefficients, Q16 rounded: w = 0.4 pi, cos 0.309017 x 65536 =
     * 20251.7 -> 20252; sin 0.951057 x 65536 = 62328.5 -> 62328
     * (62328.47); 2 D cos = 0.614944 x 65536 = 40300.97 -> 40301;
     * D^2 = 0.990025 x 65536 = 64882.3 -> 64882 ---- */
    {
        goertzel_i_t g;
        goertzel_i_init(&g, 50000.0f, 10000.0f, 0.995f, 0u, 2u);
        CHECK_EQ(g.cos_t, 20252);
        CHECK_EQ(g.sin_t, 62328);
        CHECK_EQ(g.coeff, 40301);
        CHECK_EQ(g.dd, 64882);
        CHECK_EQ(g.window, 1u);
        CHECK_EQ(g.in_shift, 2u);
        CHECK_EQ(g.lp.k, GOERTZEL_I_LP_K);
        CHECK(g.q1 == 0 && g.q2 == 0 && g.lp.tap == 0 && g.win_cnt == 0u);
    }
    /* ---- a constant input by hand: s = 512 = 2^21 in Q12.
     * step 1: q0 = 2^21, re = 2^21, im = 0, mag = 2^21 -> 512, lp 32.
     * step 2: q0 = 2^21 + (40301 x 2^21) >> 16 = 2097152 + 40301 x 32 =
     *         2097152 + 1289632 = 3386784; q2 = 2^21; re = 3386784 -
     *         32 x 20252 = 2738720; im = 32 x 62328 = 1994496; mag =
     *         2738720 + 1994496 - 997248 = 3735968 -> >> 12 = 912; tap =
     *         512 - 32 + 912 = 1392 -> lp 87. (The float variant gives the
     *         same 32, 87.) */
    {
        goertzel_i_t g;
        goertzel_i_init(&g, 50000.0f, 10000.0f, 0.995f, 1u, 2u);
        static const uint16_t x[2] = { 2048u, 2048u };
        int32_t m[2];
        CHECK_EQ(goertzel_i_block(&g, x, 2u, m), 2u);
        CHECK_EQ(m[0], 32);
        CHECK_EQ(m[1], 87);
        CHECK_EQ(g.q1, 3386784);
        CHECK_EQ(g.q2, 2097152);
    }

    /* ---- the vectors ---- */
    const uint32_t ca = run_case(argv0, "pulses_10k_n4.csv", "goertzel_10k_n4.csv",
                                 "--f 10000 --damping 0.995 --in-shift 2 --window 1",
                                 10000.0f, 0.995f, 1u, in_a);
    static int32_t out_a[NMAX];
    memcpy(out_a, out1, sizeof out_a);
    const int n_a = load_input(argv0, "pulses_10k_n4.csv", in_a);

    run_case(argv0, "pulses_10k_n4.csv", "goertzel_10k_n4_w4.csv",
             "--f 10000 --damping 0.995 --in-shift 2 --window 4",
             10000.0f, 0.995f, 4u, in_b);
    run_case(argv0, "pulses_7k_n3.csv", "goertzel_7k_n3_at10k.csv",
             "--f 10000 --damping 0.995 --in-shift 2 --window 1",
             10000.0f, 0.995f, 1u, in_b);
    const uint32_t cb = run_case(argv0, "pulses_7k_n3.csv", "goertzel_7k_n3_at7k_d099.csv",
                                 "--f 7000 --damping 0.99 --in-shift 2 --window 1",
                                 7000.0f, 0.99f, 1u, in_b);
    static int32_t out_b[NMAX];
    memcpy(out_b, out1, sizeof out_b);
    const int n_b = load_input(argv0, "pulses_7k_n3.csv", in_b);

    /* ---- independence: two instances interleaved block by block ---- */
    if (ca > 0 && cb > 0 && n_a > 0 && n_b > 0) {
        goertzel_i_t a, b;
        goertzel_i_init(&a, 50000.0f, 10000.0f, 0.995f, 1u, 2u);
        goertzel_i_init(&b, 50000.0f, 7000.0f, 0.99f, 1u, 2u);
        uint32_t wa = 0, wb = 0;
        const int blk = 64;
        for (int i = 0; i < n_a || i < n_b; i += blk) {
            if (i < n_a) {
                const int len = (n_a - i < blk) ? (n_a - i) : blk;
                wa += goertzel_i_block(&a, &in_a[i], (uint32_t)len, &out2[wa]);
            }
            if (i < n_b) {
                const int len = (n_b - i < blk) ? (n_b - i) : blk;
                wb += goertzel_i_block(&b, &in_b[i], (uint32_t)len, &out3[wb]);
            }
        }
        CHECK_EQ(wa, ca);
        CHECK_EQ(wb, cb);
        CHECK(memcmp(out_a, out2, ca * sizeof out2[0]) == 0);
        CHECK(memcmp(out_b, out3, cb * sizeof out3[0]) == 0);
    }

    printf("goertzel_i vs reference: largest deviation %.1f LSB absolute, "
           "%.2e relative (where above %.0f LSB); tolerance %.0f LSB / %.0e\n",
           max_abs_dev, max_rel_dev, ABS_TOL, ABS_TOL, REL_TOL);
    return check_summary();
}

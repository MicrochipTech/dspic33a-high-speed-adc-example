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
 * test_goertzel_f.c - host-side test for lib/goertzel_f.c (P3.3)
 *
 * Built by tools\hosttest.bat against the real src/lib/goertzel_f.c and
 * src/lib/iir1.c. The expected values are the `lp` column of the
 * Goertzel vectors written by tests/ref/goertzel_ref.py (float64 in the
 * resonator, exact integers after it), driven by the pulse trains from
 * tests/ref/wavegen_ref.py:
 *
 *   goertzel_10k_n4          fs 50 kHz, f 10 kHz, D 0.995, window 1
 *   goertzel_10k_n4_w4       the same, window 4 (every 4th value emitted)
 *   goertzel_7k_n3_at10k     7 kHz pulses through the 10 kHz resonator
 *   goertzel_7k_n3_at7k_d099 f 7 kHz, D 0.99
 *
 * TOLERANCE. The C resonator runs in float32 (24-bit mantissa, relative
 * rounding 6e-8 per operation), the reference in float64. Every step's
 * rounding acts like an input error of ~eps x |q| that the resonator
 * amplifies by its gain at resonance, about 1 / (1 - D) = 200 (D = 0.995),
 * so the state carries a relative error of order 200 x 6e-8 x (a few
 * operations) ~ 5e-5. The magnitude then goes through int32 truncation
 * and the integer low-pass (identical code on both sides), where a
 * truncation flipping by one propagates as at most 1 LSB. Hence the
 * criterion, per emitted value:
 *
 *     |c - ref| <= max(ABS_TOL, REL_TOL x |ref|)
 *     ABS_TOL = 2 LSB (truncation flips through the low-pass)
 *     REL_TOL = 1e-3  (twenty times the estimate above, so that a change
 *                      of the recurrence or a lost D^2 fails loudly while
 *                      float32 noise never does)
 *
 * The test prints the largest deviation it saw, absolute and relative,
 * so the margin is on record. Exactness is required elsewhere: the same
 * stream run as one block, as blocks of 100, after a reset, and
 * interleaved with a second instance must give bit-identical outputs
 * (same float operations in the same order).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "goertzel_f.h"
#include "check.h"
#include "vectors.h"

#define ABS_TOL 2.0
#define REL_TOL 1e-3
#define NMAX 4096

static double max_abs_dev = 0.0, max_rel_dev = 0.0;

/* i,value -> buf; returns the count or -1. */
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

/* s,mag,lp,emit,det -> the emitted lp values in order, plus the s column
 * for the in_shift sanity check; returns the row count or -1. */
static int load_vector(const char *argv0, const char *name, const char *needle,
                       int32_t *s, int32_t *lp_emitted, int *n_emitted)
{
    FILE *f = vectors_open(argv0, name);
    if (f == NULL) { return -1; }
    /* the header carries the generating command: guard against a vector
     * regenerated with other parameters than this test assumes */
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

/* Run the whole input in blocks of `blk` through one fresh instance. */
static uint32_t run_blocks(goertzel_f_t *g, const uint16_t *x, int n, int blk,
                           int32_t *out)
{
    uint32_t total = 0;
    for (int i = 0; i < n; i += blk) {
        const int len = (n - i < blk) ? (n - i) : blk;
        total += goertzel_f_block(g, &x[i], (uint32_t)len, &out[total]);
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

/* One vector: tolerance against the reference, exactness across block
 * sizes and after a reset. Leaves the input in `in` and the whole-run
 * output in out1 (count returned) for the interleave test. */
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

    /* in_shift 2: the vector's s column is x >> 2 */
    int s_ok = 1;
    for (int i = 0; i < n; i++) { if (ref_s[i] != (in[i] >> 2)) { s_ok = 0; } }
    CHECK(s_ok);

    goertzel_f_t g;
    goertzel_f_init(&g, 50000.0f, f_hz, damping, window, 2u);
    const uint32_t c1 = run_blocks(&g, in, n, n, out1);          /* one block */
    CHECK_EQ(c1, (uint32_t)n_emit);
    CHECK_EQ(c1, ((uint32_t)n + window - 1u) / window);
    compare(vector, out1, ref_lp, (int)c1);

    goertzel_f_reset(&g);                                        /* after reset */
    const uint32_t c2 = run_blocks(&g, in, n, n, out2);
    CHECK_EQ(c2, c1);
    CHECK(memcmp(out1, out2, c1 * sizeof out1[0]) == 0);

    goertzel_f_init(&g, 50000.0f, f_hz, damping, window, 2u);   /* blocks of 100 */
    const uint32_t c3 = run_blocks(&g, in, n, 100, out3);
    CHECK_EQ(c3, c1);
    CHECK(memcmp(out1, out3, c1 * sizeof out1[0]) == 0);
    return c1;
}

int main(int argc, char **argv)
{
    const char *argv0 = argc > 0 ? argv[0] : NULL;

    /* ---- coefficients ---- */
    {
        goertzel_f_t g;
        goertzel_f_init(&g, 50000.0f, 10000.0f, 0.995f, 0u, 2u);
        /* w = 0.4 pi: cos 0.309017, sin 0.951057; 2 D cos = 0.614944;
         * D^2 = 0.990025 */
        CHECK(fabs(g.cos_t - 0.309017) < 1e-6);
        CHECK(fabs(g.sin_t - 0.951057) < 1e-6);
        CHECK(fabs(g.coeff - 0.614944) < 1e-6);
        CHECK(fabs(g.dd - 0.990025) < 1e-6);
        CHECK_EQ(g.window, 1u);            /* 0 counts as 1 */
        CHECK_EQ(g.in_shift, 2u);
        CHECK_EQ(g.lp.k, GOERTZEL_LP_K);
        CHECK(g.q1 == 0.0f && g.q2 == 0.0f && g.lp.tap == 0 && g.win_cnt == 0u);
    }
    /* ---- a constant input by hand: s = 512, first three outputs.
     * step 1: q0 = 512, re = 512, im = 0, mag = 512, tap 512, lp 32.
     * step 2: q0 = 512 + 0.614944 x 512 = 826.85; q2 = 512;
     *         re = 826.85 - 512 x 0.309017 = 668.63; im = 512 x 0.951057
     *         = 486.94; mag = 668.63 + 486.94 - 243.47 = 912.10 -> 912;
     *         tap = 512 - 32 + 912 = 1392, lp = 87. */
    {
        goertzel_f_t g;
        goertzel_f_init(&g, 50000.0f, 10000.0f, 0.995f, 1u, 2u);
        static const uint16_t x[2] = { 2048u, 2048u };
        int32_t m[2];
        CHECK_EQ(goertzel_f_block(&g, x, 2u, m), 2u);
        CHECK_EQ(m[0], 32);
        CHECK_EQ(m[1], 87);
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

    /* ---- independence: two instances, different coefficients and
     * inputs, interleaved block by block; each must equal its own
     * whole-stream run bit for bit ---- */
    if (ca > 0 && cb > 0 && n_a > 0 && n_b > 0) {
        goertzel_f_t a, b;
        goertzel_f_init(&a, 50000.0f, 10000.0f, 0.995f, 1u, 2u);
        goertzel_f_init(&b, 50000.0f, 7000.0f, 0.99f, 1u, 2u);
        uint32_t wa = 0, wb = 0;
        const int blk = 64;
        for (int i = 0; i < n_a || i < n_b; i += blk) {
            if (i < n_a) {
                const int len = (n_a - i < blk) ? (n_a - i) : blk;
                wa += goertzel_f_block(&a, &in_a[i], (uint32_t)len, &out2[wa]);
            }
            if (i < n_b) {
                const int len = (n_b - i < blk) ? (n_b - i) : blk;
                wb += goertzel_f_block(&b, &in_b[i], (uint32_t)len, &out3[wb]);
            }
        }
        CHECK_EQ(wa, ca);
        CHECK_EQ(wb, cb);
        CHECK(memcmp(out_a, out2, ca * sizeof out2[0]) == 0);
        CHECK(memcmp(out_b, out3, cb * sizeof out3[0]) == 0);
    }

    printf("goertzel_f vs reference: largest deviation %.1f LSB absolute, "
           "%.2e relative (where above %.0f LSB); tolerance %.0f LSB / %.0e\n",
           max_abs_dev, max_rel_dev, ABS_TOL, ABS_TOL, REL_TOL);
    return check_summary();
}

/*
 * test_detect.c - host-side test for lib/detect.c (P3.5)
 *
 * Built by tools\hosttest.bat against the real src/lib/detect.c,
 * goertzel_f.c, goertzel_i.c and iir1.c. The detector is judged on the
 * magnitude streams the two Goertzel variants produce from the pulse
 * trains, against the `det` column and the "detections=N" header of the
 * Goertzel vectors written by tests/ref/goertzel_ref.py:
 *
 *   goertzel_10k_n4          4 pulses at the centre frequency    -> 4
 *   goertzel_10k_n4_w4       the same, every 4th value emitted   -> 4
 *   goertzel_7k_n3_at10k     7 kHz pulses, 10 kHz resonator      -> 0
 *   goertzel_7k_n3_at7k_d099 3 pulses at 7 kHz, D 0.99           -> 3
 *
 * Counts must be EXACT, and so must the positions: the detector fires on
 * the very sample the reference marks, for both the float and the
 * fixed-point Goertzel (their magnitudes differ from the reference by
 * at most 1 LSB, P3.3/P3.4, and no reference crossing happens within
 * 1 LSB of the threshold - if a future vector did, this test would say
 * so by position, not by count).
 *
 * Then the case the template got wrong through its global state: two
 * channels - float Goertzel + detector on the 10 kHz train, fixed-point
 * Goertzel + detector on the 7 kHz train - interleaved block by block,
 * each counting exactly its own pulses at its own positions; a third
 * detector on the 10 kHz resonator fed the 7 kHz train counts nothing.
 * The adaptive threshold, max_amplitude and the window are checked by
 * hand against the template's formula.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "detect.h"
#include "goertzel_f.h"
#include "goertzel_i.h"
#include "check.h"
#include "vectors.h"

#define NMAX 4096
#define HYST_HALF 32768u        /* 0.5 in Q16, the reference's default */
#define THRESHOLD 2000          /* the vectors' --threshold */

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

/* The reference's detections: their sample indices (det column), the
 * count from the header, max_amplitude from the header. */
static int load_detections(const char *argv0, const char *name, const char *needle,
                           int *pos, int *n_header, int *max_amp)
{
    FILE *f = vectors_open(argv0, name);
    if (f == NULL) { return -1; }
    CHECK(strstr(vectors_header, needle) != NULL);
    const char *h = strstr(vectors_header, "detections=");
    *n_header = h ? atoi(h + 11) : -1;
    h = strstr(vectors_header, "max_amplitude=");
    *max_amp = h ? atoi(h + 14) : -1;
    double v[5];
    int n = 0, k = 0, r;
    while ((r = vectors_row(f, v, 5)) == 1 && n < NMAX) {
        if (v[4] != 0.0) { pos[k++] = n; }
        n++;
    }
    fclose(f);
    return (r == 0) ? k : -1;
}

static uint16_t in[NMAX];
static int32_t mag[NMAX];
static int ref_pos[64], got_pos[64];

/* Feed a magnitude stream (emitted every g_window-th sample) through a
 * detector consulting every d_window-th value; record the sample index
 * of every detection. Returns the count. */
static int detect_stream(detect_t *d, const int32_t *m, uint32_t n_mag,
                         uint32_t g_window, uint32_t d_window, int *pos)
{
    int k = 0;
    for (uint32_t j = 0; j < n_mag; j++) {
        if (detect_sample(d, m[j]) && k < 64) {
            /* emitted value j sits at sample (j + 1) * g_window - 1;
             * with d_window it is consulted at every d_window-th j, and
             * that is where it fires, so the same formula holds */
            pos[k++] = (int)((j + 1u) * g_window - 1u);
        }
    }
    (void)d_window;
    return k;
}

static void check_positions(const char *what, int n_got, int n_ref)
{
    CHECK_EQ(n_got, n_ref);
    for (int i = 0; i < n_got && i < n_ref; i++) {
        check_total_++;
        if (got_pos[i] != ref_pos[i]) {
            check_failed_++;
            fprintf(stderr, "%s: detection %d at sample %d, reference %d\n",
                    what, i, got_pos[i], ref_pos[i]);
        }
    }
}

/* One vector through goertzel_f and goertzel_i, each into a fresh
 * detector; positions and counts exact; max_amplitude from the input. */
static void run_case(const char *argv0, const char *input, const char *vector,
                     const char *needle, float f_hz, float damping, uint32_t window)
{
    const int n = load_input(argv0, input, in);
    int n_hdr = -1, max_amp = -1;
    const int n_ref = load_detections(argv0, vector, needle, ref_pos, &n_hdr, &max_amp);
    CHECK(n > 0);
    CHECK(n_ref >= 0);
    CHECK_EQ(n_ref, n_hdr);                 /* det column agrees with the header */
    if (n <= 0 || n_ref < 0) { return; }

    detect_t d;

    /* float Goertzel, its window; detector window 1 */
    goertzel_f_t gf;
    goertzel_f_init(&gf, 50000.0f, f_hz, damping, window, 2u);
    uint32_t nm = goertzel_f_block(&gf, in, (uint32_t)n, mag);
    detect_init(&d, THRESHOLD, HYST_HALF, 1u);
    detect_amplitude(&d, in, (uint32_t)n, 2u);
    int k = detect_stream(&d, mag, nm, window, 1u, got_pos);
    check_positions(vector, k, n_ref);
    CHECK_EQ(d.counter, (uint32_t)n_ref);
    CHECK_EQ(d.max_amplitude, max_amp);
    /* detect_block gives the same count from the same stream */
    detect_reset(&d);
    CHECK_EQ(detect_block(&d, mag, nm), (uint32_t)n_ref);
    CHECK_EQ(d.counter, (uint32_t)n_ref);

    /* fixed-point Goertzel, the same */
    goertzel_i_t gi;
    goertzel_i_init(&gi, 50000.0f, f_hz, damping, window, 2u);
    nm = goertzel_i_block(&gi, in, (uint32_t)n, mag);
    detect_init(&d, THRESHOLD, HYST_HALF, 1u);
    k = detect_stream(&d, mag, nm, window, 1u, got_pos);
    check_positions(vector, k, n_ref);

    /* the decimation swapped: Goertzel window 1, detector window
     * `window` - the same samples are judged, so the same positions */
    if (window > 1u) {
        goertzel_f_init(&gf, 50000.0f, f_hz, damping, 1u, 2u);
        nm = goertzel_f_block(&gf, in, (uint32_t)n, mag);
        detect_init(&d, THRESHOLD, HYST_HALF, window);
        k = detect_stream(&d, mag, nm, 1u, window, got_pos);
        check_positions("swapped windows", k, n_ref);
    }
}

int main(int argc, char **argv)
{
    const char *argv0 = argc > 0 ? argv[0] : NULL;

    /* ---- init, hysteresis, re-arm by hand ---- */
    {
        detect_t d;
        detect_init(&d, 2000, HYST_HALF, 0u);
        CHECK_EQ(d.threshold, 2000);
        CHECK_EQ(d.rearm, 1000);            /* 2000 x 32768 >> 16 */
        CHECK_EQ(d.window, 1u);             /* 0 counts as 1 */
        CHECK(d.armed);
        CHECK_EQ(d.counter, 0u);
        CHECK(!detect_sample(&d, 2000));    /* not above: equal does not fire */
        CHECK(detect_sample(&d, 2001));     /* fires */
        CHECK_EQ(d.counter, 1u);
        CHECK(!d.armed);
        CHECK(!detect_sample(&d, 5000));    /* still above: no second count */
        CHECK(!detect_sample(&d, 1500));    /* below threshold, not below re-arm */
        CHECK(!d.armed);
        CHECK(!detect_sample(&d, 1000));    /* equal to re-arm: not below */
        CHECK(!d.armed);
        CHECK(!detect_sample(&d, 999));     /* re-arms, does not fire */
        CHECK(d.armed);
        CHECK(detect_sample(&d, 3000));     /* the next pulse */
        CHECK_EQ(d.counter, 2u);
        /* set_threshold keeps the armed state and moves the re-arm level */
        detect_set_threshold(&d, 400);
        CHECK_EQ(d.rearm, 200);
        CHECK(!d.armed);
        /* reset: counter, max, armed; threshold kept */
        d.max_amplitude = 77;
        detect_reset(&d);
        CHECK_EQ(d.counter, 0u);
        CHECK_EQ(d.max_amplitude, 0);
        CHECK(d.armed);
        CHECK_EQ(d.threshold, 400);
        /* a hysteresis of 0.25: 4000 x 16384 >> 16 = 1000 */
        detect_init(&d, 4000, 16384u, 1u);
        CHECK_EQ(d.rearm, 1000);
    }
    /* ---- window: only every 3rd value is consulted ---- */
    {
        detect_t d;
        detect_init(&d, 100, HYST_HALF, 3u);
        static const int32_t m[9] = { 500, 500, 0,  500, 0, 500,  0, 0, 0 };
        /* consulted: m[2] = 0 (armed, no), m[5] = 500 (fires), m[8] = 0
         * (re-arms) */
        CHECK_EQ(detect_block(&d, m, 9u), 1u);
        CHECK(d.armed);
        CHECK_EQ(d.win_cnt, 0u);
    }
    /* ---- max_amplitude and the adaptive threshold by hand: input
     * {100, 3048, 2048} >> 2 = {25, 762, 512} -> 762; scale 1.5 = 98304:
     * 762 x 98304 >> 16 = 1143; then max and counter start over ---- */
    {
        detect_t d;
        detect_init(&d, 2000, HYST_HALF, 1u);
        static const uint16_t x[3] = { 100u, 3048u, 2048u };
        detect_amplitude(&d, x, 3u, 2u);
        CHECK_EQ(d.max_amplitude, 762);
        static const uint16_t y[1] = { 400u };          /* 100: no change */
        detect_amplitude(&d, y, 1u, 2u);
        CHECK_EQ(d.max_amplitude, 762);
        d.counter = 5u;
        d.armed = false;
        CHECK_EQ(detect_adapt(&d, 98304u), 1143);
        CHECK_EQ(d.threshold, 1143);
        CHECK_EQ(d.rearm, 571);                         /* 1143 x 32768 >> 16 */
        CHECK_EQ(d.max_amplitude, 0);
        CHECK_EQ(d.counter, 0u);
        CHECK(!d.armed);                                /* adapt does not re-arm */
    }

    /* ---- the vectors, both Goertzel variants ---- */
    run_case(argv0, "pulses_10k_n4.csv", "goertzel_10k_n4.csv",
             "--f 10000 --damping 0.995 --in-shift 2 --window 1 --threshold 2000 --hyst 0.5",
             10000.0f, 0.995f, 1u);
    run_case(argv0, "pulses_10k_n4.csv", "goertzel_10k_n4_w4.csv",
             "--f 10000 --damping 0.995 --in-shift 2 --window 4 --threshold 2000 --hyst 0.5",
             10000.0f, 0.995f, 4u);
    run_case(argv0, "pulses_7k_n3.csv", "goertzel_7k_n3_at10k.csv",
             "--f 10000 --damping 0.995 --in-shift 2 --window 1 --threshold 2000 --hyst 0.5",
             10000.0f, 0.995f, 1u);
    run_case(argv0, "pulses_7k_n3.csv", "goertzel_7k_n3_at7k_d099.csv",
             "--f 7000 --damping 0.99 --in-shift 2 --window 1 --threshold 2000 --hyst 0.5",
             7000.0f, 0.99f, 1u);

    /* ---- two channels, interleaved block by block ---- */
    {
        static uint16_t in_a[NMAX], in_b[NMAX];
        static int ref_a[64], ref_b[64], pos_a[64], pos_b[64];
        static int32_t ma[128], mb[128];
        int hdr, amp;
        const int n_a = load_input(argv0, "pulses_10k_n4.csv", in_a);
        const int n_b = load_input(argv0, "pulses_7k_n3.csv", in_b);
        const int k_a = load_detections(argv0, "goertzel_10k_n4.csv", "--f 10000",
                                        ref_a, &hdr, &amp);
        const int k_b = load_detections(argv0, "goertzel_7k_n3_at7k_d099.csv", "--f 7000",
                                        ref_b, &hdr, &amp);
        CHECK(n_a > 0 && n_b > 0 && k_a == 4 && k_b == 3);
        if (n_a > 0 && n_b > 0 && k_a == 4 && k_b == 3) {
            goertzel_f_t ga;                /* channel A: float, 10 kHz */
            goertzel_i_t gb;                /* channel B: fixed, 7 kHz */
            goertzel_f_t gc;                /* channel C: 10 kHz resonator on B's signal */
            detect_t da, db, dc;
            goertzel_f_init(&ga, 50000.0f, 10000.0f, 0.995f, 1u, 2u);
            goertzel_i_init(&gb, 50000.0f, 7000.0f, 0.99f, 1u, 2u);
            goertzel_f_init(&gc, 50000.0f, 10000.0f, 0.995f, 1u, 2u);
            detect_init(&da, THRESHOLD, HYST_HALF, 1u);
            detect_init(&db, THRESHOLD, HYST_HALF, 1u);
            detect_init(&dc, THRESHOLD, HYST_HALF, 1u);
            int na = 0, nb = 0;
            const int blk = 64;
            for (int i = 0; i < n_a || i < n_b; i += blk) {
                if (i < n_a) {
                    const int len = (n_a - i < blk) ? (n_a - i) : blk;
                    const uint32_t m = goertzel_f_block(&ga, &in_a[i], (uint32_t)len, ma);
                    detect_amplitude(&da, &in_a[i], (uint32_t)len, 2u);
                    for (uint32_t j = 0; j < m; j++) {
                        if (detect_sample(&da, ma[j]) && na < 64) { pos_a[na++] = i + (int)j; }
                    }
                }
                if (i < n_b) {
                    const int len = (n_b - i < blk) ? (n_b - i) : blk;
                    const uint32_t m = goertzel_i_block(&gb, &in_b[i], (uint32_t)len, mb);
                    detect_amplitude(&db, &in_b[i], (uint32_t)len, 2u);
                    for (uint32_t j = 0; j < m; j++) {
                        if (detect_sample(&db, mb[j]) && nb < 64) { pos_b[nb++] = i + (int)j; }
                    }
                    const uint32_t mc = goertzel_f_block(&gc, &in_b[i], (uint32_t)len, mb);
                    (void)detect_block(&dc, mb, mc);
                }
            }
            CHECK_EQ(na, 4);
            CHECK_EQ(nb, 3);
            CHECK_EQ(da.counter, 4u);
            CHECK_EQ(db.counter, 3u);
            CHECK_EQ(dc.counter, 0u);
            CHECK(memcmp(pos_a, ref_a, 4 * sizeof pos_a[0]) == 0);
            CHECK(memcmp(pos_b, ref_b, 3 * sizeof pos_b[0]) == 0);
            CHECK_EQ(da.max_amplitude, 762);
            CHECK_EQ(db.max_amplitude, 762);
            /* a reset of A leaves B counting on */
            detect_reset(&da);
            CHECK_EQ(da.counter, 0u);
            CHECK_EQ(db.counter, 3u);
            CHECK(db.armed);                /* the last gap re-armed it */
        }
    }

    return check_summary();
}

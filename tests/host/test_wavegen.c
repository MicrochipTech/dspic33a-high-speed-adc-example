/*
 * test_wavegen.c - host-side test for lib/wavegen.c (P3.6)
 *
 * Built by tools\hosttest.bat against the real src/lib/wavegen.c. The
 * expected tables come from tests/ref/wavegen_ref.py (float64, the
 * script's formula), all at n = 512, play 500 kHz, f0 10 kHz, harmonics
 * 0.2 0.4 0.1 0 0 0, decay 1000:
 *
 *   wavegen_0_1023          amplitude 1.0, 0 .. 1023   (the script's range)
 *   wavegen_0_1023_amp05    amplitude 0.5, 0 .. 1023   (maximum 512)
 *   wavegen_205_3890        amplitude 1.0, 205 .. 3890 (the DAC range, ATDF)
 *
 * TOLERANCE: +-1 LSB per value. The C table is float32 (relative
 * rounding 6e-8; the phase argument reaches 450 rad, so its rounding is
 * ~3e-5 rad, i.e. ~0.03 LSB of a 10-bit table after scaling), the
 * reference float64; the only difference that can reach a whole LSB is
 * a value that sits within that error of a rounding boundary, and the
 * two round half-way cases differently (C: up; numpy: to even). The
 * test prints how many values differ at all.
 *
 * Every table must also span exactly its range (min = out_min, max =
 * out_min + span x amplitude) - that is the normalisation, not the
 * rounding. Then the snap option, and every error code.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "wavegen.h"
#include "check.h"
#include "vectors.h"

#define NMAX 4096

static int load_table(const char *argv0, const char *name, const char *needle,
                      uint16_t *buf)
{
    FILE *f = vectors_open(argv0, name);
    if (f == NULL) { return -1; }
    CHECK(strstr(vectors_header, needle) != NULL);
    double v[2];
    int n = 0, r;
    while ((r = vectors_row(f, v, 2)) == 1 && n < NMAX) { buf[n++] = (uint16_t)v[1]; }
    fclose(f);
    return (r == 0) ? n : -1;
}

static wavegen_cfg_t base_cfg(void)
{
    wavegen_cfg_t c;
    c.n = 512u;
    c.play_hz = 500000u;
    c.f0_hz = 10000.0f;
    c.harm[0] = 0.2f; c.harm[1] = 0.4f; c.harm[2] = 0.1f;
    c.harm[3] = 0.0f; c.harm[4] = 0.0f; c.harm[5] = 0.0f;
    c.decay = 1000.0f;
    c.amplitude = 1.0f;
    c.out_min = 0u;
    c.out_max = 1023u;
    return c;
}

static uint16_t ref[NMAX], tab[NMAX];

/* One vector: +-1 LSB per value, exact range. */
static void run_case(const char *argv0, const char *name, const char *needle,
                     float amplitude, uint16_t out_min, uint16_t out_max)
{
    const int n = load_table(argv0, name, needle, ref);
    CHECK_EQ(n, 512);
    if (n != 512) { return; }

    wavegen_cfg_t c = base_cfg();
    c.amplitude = amplitude;
    c.out_min = out_min;
    c.out_max = out_max;
    float f0 = 0.0f;
    memset(tab, 0xAA, sizeof tab);
    CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_OK);
    CHECK(f0 == 10000.0f);

    int differ = 0, worst = 0;
    uint16_t lo = 0xFFFFu, hi = 0u;
    for (int i = 0; i < n; i++) {
        const int d = (int)tab[i] - (int)ref[i];
        const int ad = d < 0 ? -d : d;
        check_total_++;
        if (ad > 1) {
            check_failed_++;
            fprintf(stderr, "%s [%d]: c %u, ref %u\n", name, i, tab[i], ref[i]);
        }
        if (ad > 0) { differ++; }
        if (ad > worst) { worst = ad; }
        if (tab[i] < lo) { lo = tab[i]; }
        if (tab[i] > hi) { hi = tab[i]; }
    }
    CHECK_EQ(lo, out_min);
    CHECK_EQ(hi, (uint16_t)(out_min + (uint16_t)floorf((float)(out_max - out_min) * amplitude + 0.5f)));
    CHECK_EQ(tab[512], 0xAAAAu);          /* nothing written past n */
    printf("%s: %d of %d values differ from the reference, worst %d LSB\n",
           name, differ, n, worst);
}

int main(int argc, char **argv)
{
    const char *argv0 = argc > 0 ? argv[0] : NULL;

    /* ---- the vectors ---- */
    run_case(argv0, "wavegen_0_1023.csv",
             "--decay 1000 --amplitude 1.0 --out-min 0 --out-max 1023",
             1.0f, 0u, 1023u);
    run_case(argv0, "wavegen_0_1023_amp05.csv",
             "--decay 1000 --amplitude 0.5 --out-min 0 --out-max 1023",
             0.5f, 0u, 1023u);
    run_case(argv0, "wavegen_205_3890.csv",
             "--decay 1000 --amplitude 1.0 --out-min 205 --out-max 3890",
             1.0f, 205u, 3890u);

    /* ---- snap: 10 kHz x 512 / 500 kHz = 10.24 periods -> 10 periods =
     * 9765.625 Hz. Without decay and harmonics the table then wraps
     * without a jump: the step from the last sample back to the first is
     * the same step as from the first to the second (both are one sample
     * of the sine at its zero crossing). Unsnapped, the wrap sits 0.24
     * periods off and jumps by hundreds of counts. ---- */
    {
        wavegen_cfg_t c = base_cfg();
        c.decay = 0.0f;
        c.harm[0] = 0.0f; c.harm[1] = 0.0f; c.harm[2] = 0.0f;
        CHECK(fabsf(wavegen_snap_hz(&c) - 9765.625f) < 1e-3f);

        float f0 = 0.0f;
        CHECK_EQ(wavegen_fill(&c, tab, true, &f0), WAVEGEN_OK);
        CHECK(fabsf(f0 - 9765.625f) < 1e-3f);
        const int step_in = (int)tab[1] - (int)tab[0];
        const int step_wrap = (int)tab[0] - (int)tab[511];
        CHECK(step_in > 30);                           /* sin(2 pi 10/512) x 511.5 = 62 */
        CHECK(step_wrap - step_in <= 2 && step_in - step_wrap <= 2);

        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_OK);
        CHECK(f0 == 10000.0f);
        const int jump = (int)tab[511] - (int)tab[0];
        CHECK(jump > 300 || jump < -300);              /* ~0.98 x 511 */

        /* snap never goes below one period */
        c.f0_hz = 100.0f;                              /* 0.1 periods */
        CHECK(fabsf(wavegen_snap_hz(&c) - 976.5625f) < 1e-3f);
        /* a whole number already: unchanged */
        c.f0_hz = 19531.25f;                           /* exactly 20 periods */
        CHECK(fabsf(wavegen_snap_hz(&c) - 19531.25f) < 1e-3f);
        /* f0_used may be NULL */
        CHECK_EQ(wavegen_fill(&c, tab, true, NULL), WAVEGEN_OK);
    }

    /* ---- error codes; the table is untouched on each ---- */
    {
        float f0 = 1.0f;
        memset(tab, 0x55, sizeof tab);
        wavegen_cfg_t c = base_cfg(); c.n = 1u;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_N);
        c = base_cfg(); c.play_hz = 0u;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_RATE);
        c = base_cfg(); c.f0_hz = 0.0f;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_F0);
        c = base_cfg(); c.f0_hz = 250000.0f;           /* play_hz / 2 */
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_F0);
        c = base_cfg(); c.decay = -1.0f;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_DECAY);
        c = base_cfg(); c.amplitude = 0.0f;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_AMPLITUDE);
        c = base_cfg(); c.amplitude = 1.5f;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_AMPLITUDE);
        c = base_cfg(); c.out_min = 1023u; c.out_max = 1023u;
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_RANGE);
        c = base_cfg(); c.decay = 1e12f;               /* every sample after t = 0 is 0 */
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_E_FLAT);
        CHECK(f0 == 1.0f);                             /* never written */
        CHECK_EQ(tab[0], 0x5555u);
        CHECK_EQ(tab[511], 0x5555u);
        /* the smallest table */
        c = base_cfg(); c.n = 2u; c.decay = 0.0f; c.f0_hz = 100000.0f;   /* 0.4 periods */
        CHECK_EQ(wavegen_fill(&c, tab, false, &f0), WAVEGEN_OK);
        CHECK_EQ(tab[0], 0u);                          /* sin 0 is the minimum */
        CHECK_EQ(tab[1], 1023u);                       /* sin(0.8 pi) + harmonics the maximum */
    }

    return check_summary();
}

/*
 * test_tri_eval.c - host-side test for lab/tri_eval.c (fit_line, tri_eval,
 * tri_grid_ok), the chain test's triangle evaluator.
 *
 * Built by tools\hosttest.bat with the installed MinGW gcc, against the
 * real src/lab/tri_eval.c - not a reimplementation.
 *
 * Two jobs:
 *
 * 1. Without arguments: the statistics test. It repeats the host test the
 *    evaluator was tuned with on 25.09.2026 before its first board run
 *    (CLAUDE.md, docs/CHAIN-TEST-PLAN.md section 9, HARDWARE-LOG): synthetic
 *    triangle windows with the DAC's DNL of +-5 LSB (Table 40-42), Gaussian
 *    noise, and a modelled first-order DAC/ADC settling filter, clean and
 *    with a single lost or repeated sample. The figures documented there
 *    are the acceptance criteria: NO false alarm in 2100 clean windows, and
 *    a single lost or repeated sample found in 96-100 % of windows. The
 *    generator is a C port of synth() in tools/eval_chain.py with its own
 *    fixed-seed PRNG, so every run sees the same windows.
 *
 * 2. `test_tri_eval --eval`: reads windows from stdin, one per line as
 *    decimal samples separated by blanks, and prints one line per window
 *    with every field of tri_t plus the grid verdict. This is what
 *    tests/host/test_tri_eval_xcheck.py (P2.4) drives, to compare this C
 *    code with the Python port in tools/eval_chain.py on identical windows.
 *    Floats are printed with %.9g, which round-trips a float exactly.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tri_eval.h"
#include "check.h"

/* ------------------------------------------------------------------ *
 * A small deterministic PRNG (xorshift64*), uniform and Gaussian draws
 * ------------------------------------------------------------------ */
static uint64_t rng_s;

static void rng_seed(uint64_t seed)
{
    rng_s = seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
    if (rng_s == 0u) { rng_s = 1u; }
}

static uint64_t rng_next(void)
{
    rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27;
    return rng_s * 0x2545F4914F6CDD1Dull;
}

static double rng_uniform(void)          /* [0, 1) */
{
    return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

static int rng_randint(int lo, int hi)   /* lo..hi inclusive */
{
    return lo + (int)(rng_next() % (uint64_t)(hi - lo + 1));
}

static double rng_gauss(double sigma)    /* Box-Muller, one draw per call */
{
    double u1 = rng_uniform();
    if (u1 < 1e-300) { u1 = 1e-300; }
    const double u2 = rng_uniform();
    return sigma * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* ------------------------------------------------------------------ *
 * The synthetic window, a port of synth() in tools/eval_chain.py:
 * a triangle between 300 and 3800 counts with `slope` samples per
 * slope, starting at `phase` samples into it; every code carries its
 * own DNL of -5..+5 LSB; a first-order filter with time constant `tau`
 * samples (0 = none) models the DAC's and the ADC's settling; Gaussian
 * noise of 1.5 LSB; one sample lost at index `drop` (< 0: none) or
 * repeated at index `dup` (< 0: none).
 * ------------------------------------------------------------------ */
#define LO 300.0
#define HI 3800.0

static double tri_wave(double t, double slope)
{
    const double p = fmod(t + 4000.0 * slope, 2.0 * slope);
    return (p < slope) ? (LO + (HI - LO) * p / slope)
                       : (HI - (HI - LO) * (p - slope) / slope);
}

static void synth(uint16_t *out, uint32_t n, double slope, double phase,
                  int drop, int dup, double tau, uint64_t seed)
{
    static int dnl[4096];
    rng_seed(seed);
    for (int i = 0; i < 4096; i++) { dnl[i] = rng_randint(-5, 5); }
    const double al = (tau > 0.0) ? (1.0 - exp(-1.0 / tau)) : 1.0;
    double yf = LO, t = phase - 200.0;
    while (t < phase) { yf += al * (tri_wave(t, slope) - yf); t += 1.0; }
    uint32_t len = 0u;
    while (len < n) {
        if ((drop >= 0) && (len == (uint32_t)drop)) { t += 1.0; drop = -1; }
        const int code = (int)floor(tri_wave(t, slope) + 0.5);
        yf += al * ((double)(code + dnl[code & 4095]) - yf);
        double v = yf + rng_gauss(1.5) + 0.5;
        if (v < 0.0) { v = 0.0; }
        if (v > 4095.0) { v = 4095.0; }
        out[len++] = (uint16_t)(int)v;
        if ((dup >= 0) && (len == (uint32_t)dup) && (len < n)) { out[len] = out[len - 1u]; len++; dup = -1; }
        t += 1.0;
    }
}

/* ------------------------------------------------------------------ *
 * 1. Direct checks of fit_line and of tri_eval's early returns
 * ------------------------------------------------------------------ */
#define WIN 2048u
static uint16_t win[WIN + 1u];

static void direct_checks(void)
{
    double m = 0.0, c = 0.0;

    /* an exact line y = 3 i + 7: slope and intercept come back exactly */
    for (uint32_t i = 0; i < 100u; i++) { win[i] = (uint16_t)(3u * i + 7u); }
    CHECK(fit_line(win, 10u, 60u, &m, &c));
    CHECK(fabs(m - 3.0) < 1e-9);
    CHECK(fabs(c - 7.0) < 1e-9);
    /* a falling line y = 4000 - 2 i */
    for (uint32_t i = 0; i < 100u; i++) { win[i] = (uint16_t)(4000u - 2u * i); }
    CHECK(fit_line(win, 0u, 99u, &m, &c));
    CHECK(fabs(m + 2.0) < 1e-9);
    CHECK(fabs(c - 4000.0) < 1e-9);
    /* refused: b <= a, or fewer than 5 samples apart */
    CHECK(!fit_line(win, 10u, 10u, &m, &c));
    CHECK(!fit_line(win, 10u, 5u, &m, &c));
    CHECK(!fit_line(win, 10u, 14u, &m, &c));
    CHECK(fit_line(win, 10u, 15u, &m, &c));

    /* tri_eval on fewer than 16 samples: min/max/pp only, no verdict */
    tri_t r;
    for (uint32_t i = 0; i < 15u; i++) { win[i] = (uint16_t)(100u + i); }
    tri_eval(win, 15u, &r);
    CHECK_EQ(r.mn, 100u); CHECK_EQ(r.mx, 114u); CHECK_EQ(r.pp, 14u);
    CHECK_EQ(r.tps, 0u); CHECK_EQ(r.slip_n, 0u);
    CHECK(r.slip == 99.0f);
    CHECK(!tri_grid_ok(&r));

    /* a constant window: no turning point, no verdict */
    for (uint32_t i = 0; i < WIN; i++) { win[i] = 2000u; }
    tri_eval(win, WIN, &r);
    CHECK_EQ(r.pp, 0u); CHECK_EQ(r.tps, 0u);
    CHECK(!tri_grid_ok(&r));

    /* an exact noiseless triangle, 100 samples per slope: both directions
     * seen, every slope 100 long, no slip, grid ok */
    for (uint32_t i = 0; i < WIN; i++) {
        const uint32_t p = i % 200u;
        win[i] = (uint16_t)((p < 100u) ? (500u + 30u * p) : (3500u - 30u * (p - 100u)));
    }
    tri_eval(win, WIN, &r);
    CHECK_EQ(r.mn, 500u); CHECK_EQ(r.mx, 3500u);
    CHECK(r.n_up >= 1u); CHECK(r.n_dn >= 1u);
    CHECK(fabs((double)r.l_up - 100.0) < 0.01);
    CHECK(fabs((double)r.l_dn - 100.0) < 0.01);
    CHECK(r.slip < 0.01f);
    CHECK_EQ(r.slip_k, 4u);
    CHECK(fabs((double)r.step - 30.0) < 0.01);
    CHECK(!r.step_checked);                  /* 30 < STEP_CHECK_LSB */
    CHECK(tri_grid_ok(&r));

    /* the same triangle with one sample lost in the middle: caught */
    memmove(&win[1000], &win[1001], (WIN - 1001u) * sizeof win[0]);
    tri_eval(win, WIN - 1u, &r);
    CHECK(!tri_grid_ok(&r));
    CHECK(r.slip > 0.9f);                    /* a whole sample over two periods */
}

/* ------------------------------------------------------------------ *
 * 2. The statistics: clean windows, one lost sample, one repeated sample
 * ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    double slope, tau;
} cfg_t;

/* Slopes as the chain test runs them: ~128 samples per slope at 8 MSPS
 * (about 27 LSB per sample - below STEP_CHECK_LSB, so only the slip
 * verdict sees a fault), the same at 40 MSPS with the DAC's settling
 * modelled (tau 8 samples), and the steep short slopes of the low rates,
 * where the steps are checked as well. */
static const cfg_t cfgs[] = {
    { "slope 127.5, no filter  (~27 LSB/sample, slip only)   ", 127.5, 0.0 },
    { "slope 126.8, tau 8      (40 MSPS with the DAC filter) ", 126.8, 8.0 },
    { "slope 64.0, no filter   (~55 LSB/sample, steps checked)", 64.0, 0.0 },
    { "slope 29.0, no filter   (~120 LSB/sample, steps checked)", 29.0, 0.0 },
};
#define NCFG        (sizeof cfgs / sizeof cfgs[0])
#define PER_CFG     525u                    /* 4 x 525 = 2100 clean windows */
#define DETECT_MIN  96.0                    /* per cent, CLAUDE.md          */
#define FAULT_LO    512                     /* fault positions: the middle  */
#define FAULT_HI    1535                    /* half of the 2048 window      */

static void statistics(void)
{
    uint32_t clean_total = 0u, clean_alarms = 0u;
    tri_t r;
    printf("%-58s %10s %10s %10s\n", "configuration", "false", "lost", "repeated");
    printf("%-58s %10s %10s %10s\n", "", "alarms", "found", "found");
    for (uint32_t c = 0; c < NCFG; c++) {
        uint32_t alarms = 0u, lost_found = 0u, dup_found = 0u;
        for (uint32_t k = 0; k < PER_CFG; k++) {
            const double phase = (double)k * 2.0 * cfgs[c].slope / (double)PER_CFG;
            const uint64_t seed = 1000u * (c + 1u) + k + 1u;
            /* clean */
            synth(win, WIN, cfgs[c].slope, phase, -1, -1, cfgs[c].tau, seed);
            tri_eval(win, WIN, &r);
            if (!tri_grid_ok(&r)) {
                alarms++;
                fprintf(stderr, "false alarm: cfg %u k %u phase %.2f: tps %u up %u dn %u "
                        "slip %.3f (k %u, n %u) zero %u dbl %u step %.1f\n",
                        (unsigned)c, (unsigned)k, phase, (unsigned)r.tps, (unsigned)r.n_up,
                        (unsigned)r.n_dn, (double)r.slip, (unsigned)r.slip_k,
                        (unsigned)r.slip_n, (unsigned)r.zero, (unsigned)r.dbl, (double)r.step);
            }
            /* one lost sample, somewhere in the middle half of the window
             * (FAULT_LO..FAULT_HI). Not nearer the ends: the slip verdict
             * counts only turning points between two full slopes, so a
             * fault in the window's first or last slope shifts every
             * counted turning point alike and is invisible BY DESIGN
             * (tri_eval.c, step 5). With faults anywhere in [200, 1848]
             * the ~128-sample slope without filter measured 95.4 % /
             * 95.8 % (27.09.2026) - the blind end segments, about 6 % of
             * that range, account for the shortfall. */
            rng_seed(seed ^ 0xABCDu);
            const int at = FAULT_LO + rng_randint(0, FAULT_HI - FAULT_LO);
            synth(win, WIN, cfgs[c].slope, phase, at, -1, cfgs[c].tau, seed);
            tri_eval(win, WIN, &r);
            if (!tri_grid_ok(&r)) { lost_found++; }
            /* one repeated sample */
            synth(win, WIN, cfgs[c].slope, phase, -1, at, cfgs[c].tau, seed);
            tri_eval(win, WIN, &r);
            if (!tri_grid_ok(&r)) { dup_found++; }
        }
        const double lost_pct = 100.0 * (double)lost_found / (double)PER_CFG;
        const double dup_pct  = 100.0 * (double)dup_found  / (double)PER_CFG;
        printf("%-58s %4u/%-5u %7.1f %% %7.1f %%\n", cfgs[c].name,
               (unsigned)alarms, (unsigned)PER_CFG, lost_pct, dup_pct);
        clean_total += PER_CFG; clean_alarms += alarms;
        CHECK(lost_pct >= DETECT_MIN);
        CHECK(dup_pct >= DETECT_MIN);
    }
    printf("clean windows: %u, false alarms: %u\n", (unsigned)clean_total, (unsigned)clean_alarms);
    CHECK(clean_total >= 2100u);
    CHECK_EQ(clean_alarms, 0u);
}

/* ------------------------------------------------------------------ *
 * --eval: windows from stdin, one result line per window
 * ------------------------------------------------------------------ */
static int eval_stdin(void)
{
    static char line[65536];                /* 2048 samples x "4095 " = 10 KB */
    static uint16_t w[8192];
    while (fgets(line, sizeof line, stdin) != NULL) {
        uint32_t n = 0u;
        char *p = line;
        for (;;) {
            char *end;
            while ((*p == ' ') || (*p == '\t')) { p++; }
            if ((*p == '\0') || (*p == '\r') || (*p == '\n')) { break; }
            const unsigned long v = strtoul(p, &end, 10);
            if ((end == p) || (n >= 8192u)) { fprintf(stderr, "bad window\n"); return 2; }
            w[n++] = (uint16_t)v;
            p = end;
        }
        if (n == 0u) { continue; }
        tri_t r;
        tri_eval(w, n, &r);
        printf("mn=%u mx=%u pp=%u tps=%u n_up=%u n_dn=%u l_up=%.9g l_dn=%.9g dev=%.9g "
               "step=%.9g zero=%u dbl=%u slip=%.9g slip_k=%u slip_n=%u step_checked=%d "
               "overflow=%d grid_ok=%d\n",
               (unsigned)r.mn, (unsigned)r.mx, (unsigned)r.pp, (unsigned)r.tps,
               (unsigned)r.n_up, (unsigned)r.n_dn, (double)r.l_up, (double)r.l_dn,
               (double)r.dev, (double)r.step, (unsigned)r.zero, (unsigned)r.dbl,
               (double)r.slip, (unsigned)r.slip_k, (unsigned)r.slip_n,
               r.step_checked ? 1 : 0, r.overflow ? 1 : 0, tri_grid_ok(&r) ? 1 : 0);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if ((argc == 2) && (strcmp(argv[1], "--eval") == 0)) { return eval_stdin(); }
    direct_checks();
    statistics();
    return check_summary();
}

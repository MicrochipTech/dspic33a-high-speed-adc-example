/*
 * chaintest.c - the chain test (chaintest.h, docs/CHAIN-TEST-PLAN.md)
 *
 * WHAT IT CHECKS
 *
 * The example's sentence, link by link and then as a whole:
 *
 *   SCCP1 (CLKGEN13, 160 MHz) -> ADC core 5, Single Conversion, one
 *   conversion per trigger -> DMA0, Repeated Continuous, one transfer
 *   per conversion -> ping-pong buffer -> the CPU processes each half
 *
 * with DAC2 as the signal, on DACOUT2 = RA8 = AD5AN3 (ANALYSIS.md C.11).
 * Stages S0..S9, from what cannot fail to what probably will:
 *
 *   S0  preconditions: Timer1, clock tree measured by the clock monitor,
 *       core 5 calibration and channels, RA8 analog
 *   S1  SCCP1 alone: its clock against Timer1, its period by interrupts
 *   S2  SCCP1 -> ADC at 1..100 kHz, both ends counted by the CPU; the DAC
 *       as a static transfer and as a CPU-stepped sequence
 *   S3  ADC -> DMA -> buffer at 100 kHz, CPU-stepped, three blocks
 *   S4  every rate of the ladder: triggers against transfers, overrun
 *   S5  every rate: the DAC triangle in the data, turning points by line
 *       fits, slope lengths against each other and against the model
 *   S6  every rate that passed S4: 1 s of stream with the CPU processing
 *   S7  start, stop, restart, rate change
 *   S8  the old open questions with the new instruments (CLKGEN6 divider,
 *       CLKGEN6 off, back-to-back repeats)
 *   S9  the attempt: the chain as the example runs it, 15 s at the best
 *       rate and at 8 MSPS, and the registers it ran with
 *
 * THE LOG
 *
 * Every result is one line "@S<stage>.<n> key=value ... -> VERDICT",
 * VERDICT one of PASS, FAIL, SKIP, INFO. A window whose verdict is FAIL
 * follows as "@DUMP" lines, 64 samples each in 3-digit hex. "@SUM" lines
 * and "@END" close the run. tools/eval_chain.py reads it back. Fields
 * named *_x100 / *_x1000 are fixed point.
 *
 * WHAT IT CANNOT SAY
 *
 * Triggers are not counted in hardware at the high rates - a second DMA
 * channel on the SCCP1 event would double the DMA load it is trying to
 * measure. They are computed from Timer1 instead: both clocks come from
 * the same FRC through two PLLs, so their ratio is exact, and the only
 * uncertainty is where the Timer1 reads fall, +-3 ticks, i.e. a few
 * triggers at 40 MSPS. A single lost or repeated sample is therefore not
 * visible in S4's count; it is visible in S5, where one slope comes out
 * one sample short or long.
 */

#include <xc.h>
#include <stddef.h>
#include <stdbool.h>
#include "board.h"
#include "chaintest.h"
#include "capture.h"
#include "adc.h"
#include "sccp.h"
#include "dac.h"
#include "clock.h"
#include "timebase.h"
#include "console.h"
#include "diag.h"
#include "dma.h"

/* ------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------ */
#define CHAIN_CORE        DAC_ADC_CORE      /* 5: DACOUT2 is AD5AN3        */
#define CHAIN_PINSEL      DAC_ADC_PINSEL    /* 3: RA8                      */
#define CHAIN_SAMC        0u                /* 0.5 TAD, the shortest       */
#define TRIG_HZ_NOMINAL   160000000u        /* CLKGEN13 = PLL1 out / 2     */
#define CPU_PER_TICK      16u               /* 200 MHz / 12.5 MHz          */

/* The rate ladder: SCCP1 period in ticks of its 160 MHz clock, slowest
 * first. 100 kSPS, then 1, 4, 8, 10, 16, 20, 26.7, 32, 40 MSPS
 * (ANALYSIS.md C.12.9). */
static const uint16_t ladder_n[] = { 1600u, 160u, 40u, 20u, 16u, 10u, 8u, 6u, 5u, 4u };
#define LADDER_LEN   (sizeof ladder_n / sizeof ladder_n[0])
#define IDX_8MSPS    3u                     /* N = 20                      */
#define IDX_1MSPS    1u                     /* N = 160                     */

/* ------------------------------------------------------------------ *
 * State of one run
 * ------------------------------------------------------------------ */
typedef enum { V_PASS = 0, V_FAIL, V_SKIP, V_INFO } verdict_t;

static uint32_t g_stage;                    /* stage being run             */
static uint16_t tally[10][4];               /* per stage, per verdict      */
static uint32_t g_trig_hz = TRIG_HZ_NOMINAL;/* measured in S1              */
static bool     g_setup_ok;                 /* clock tree and core         */
static bool     g_trigger_ok;               /* S2: SCCP1 reaches the ADC   */
static bool     s4_ok[LADDER_LEN], s4_data[LADDER_LEN];
static bool     s5_ok[LADDER_LEN], s6_ok[LADDER_LEN];
static bool     s4_run, s5_run, s6_run;     /* stage ran in this call      */

/* The static transfer DAC code -> ADC count from S2, used to predict
 * what the ADC must read for a code. Unity until measured. */
static float    g_gain = 1.0f, g_offs = 0.0f;
static uint32_t g_tol = 150u;               /* LSB, set from the fit       */

/* ------------------------------------------------------------------ *
 * The log line
 *
 * One buffer, filled field by field, printed at the end. Every field is
 * bounded (ln_s stops LN_RESERVE short of the end), so a long line is
 * cut, never an overrun; the reserve holds the verdict and CRLF.
 * ------------------------------------------------------------------ */
#define LN_SIZE      256u
#define LN_RESERVE   16u
static char     ln[LN_SIZE];
static uint32_t ln_len;

static void ln_s(const char *s)
{
    while ((*s != '\0') && (ln_len < (LN_SIZE - LN_RESERVE))) { ln[ln_len++] = *s++; }
}

static void fmt_u(char *out, uint32_t v)
{
    char tmp[11];
    uint32_t i = 0;
    do { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u);
    while (i > 0u) { *out++ = tmp[--i]; }
    *out = '\0';
}

static void ln_u(const char *k, uint32_t v)
{
    char t[12];
    fmt_u(t, v);
    ln_s(" "); ln_s(k); ln_s("="); ln_s(t);
}

static void ln_i(const char *k, int32_t v)
{
    char t[13];
    if (v < 0) { t[0] = '-'; fmt_u(&t[1], (uint32_t)(-(v + 1)) + 1u); }
    else       { fmt_u(t, (uint32_t)v); }
    ln_s(" "); ln_s(k); ln_s("="); ln_s(t);
}

static void ln_x(const char *k, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char t[11];
    t[0] = '0'; t[1] = 'x';
    for (uint32_t i = 0; i < 8u; i++) { t[2u + i] = hex[(v >> (28u - 4u * i)) & 0xFu]; }
    t[10] = '\0';
    ln_s(" "); ln_s(k); ln_s("="); ln_s(t);
}

/* A float as a signed integer times 100 or 1000. */
static void ln_f(const char *k, float v, uint32_t scale)
{
    const float x = v * (float)scale;
    ln_i(k, (int32_t)((x >= 0.0f) ? (x + 0.5f) : (x - 0.5f)));
}

static void ln_begin(uint32_t n)
{
    ln_len = 0u;
    ln_s("@S");
    char t[12];
    fmt_u(t, g_stage); ln_s(t);
    ln_s(".");
    fmt_u(t, n); ln_s(t);
}

static void ln_end(verdict_t v)
{
    static const char *const txt[] = { " -> PASS", " -> FAIL", " -> SKIP", " -> INFO" };
    const char *s = txt[v];
    while (*s != '\0') { ln[ln_len++] = *s++; }
    ln[ln_len++] = '\r'; ln[ln_len++] = '\n'; ln[ln_len] = '\0';
    console_puts(ln);
    if (g_stage < 10u) { tally[g_stage][v]++; }
}

static void say(const char *s) { console_puts(s); }

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */
static void wait_ticks(uint32_t t)
{
    const uint32_t t0 = timebase_ticks();
    while ((timebase_ticks() - t0) < t) { }
}

#define TICKS_PER_MS  (TIMEBASE_HZ / 1000u)

static uint32_t rate_hz(uint32_t n)       { return g_trig_hz / n; }
static uint32_t ksps_of(uint32_t n)       { return (g_trig_hz / 1000u + n / 2u) / n; }

/* Triggers expected in a Timer1 window, and the tolerance of the count:
 * +-3 Timer1 ticks of read placement, plus one for the first period. */
static uint64_t expected_triggers(uint32_t window_ticks, uint32_t n)
{
    return ((uint64_t)window_ticks * g_trig_hz) / ((uint64_t)n * TIMEBASE_HZ);
}

static uint32_t trigger_tol(uint32_t n)
{
    return 2u + (uint32_t)((3ull * g_trig_hz + (uint64_t)n * TIMEBASE_HZ - 1u) /
                           ((uint64_t)n * TIMEBASE_HZ));
}

static uint32_t absdiff64(uint64_t a, uint64_t b)
{
    const uint64_t d = (a > b) ? (a - b) : (b - a);
    return (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)d;
}

/* ------------------------------------------------------------------ *
 * The ADC interrupt at low rates (adc.h: adc_ch0_event)
 *
 * Counts results, keeps the first S2_KEEP of them, and in the CPU-
 * stepped mode writes the DAC code for the NEXT sample right after this
 * one was converted. At 100 kHz there are 10 us until the next trigger,
 * the DAC settles in 0.6 us (Table 40-43).
 *
 * The sequence is pseudo-random with a large step, so that neighbouring
 * codes are always at least 1237 apart: a sample that belongs to the
 * code before or after its own can never pass as right.
 * ------------------------------------------------------------------ */
#define S2_KEEP      256u
#define STEP_LOW     0x100u
#define STEP_SPAN    0xE00u                 /* 0x100..0xEFF                */
static volatile uint32_t adc_events = 0;
static volatile uint16_t adc_keep[S2_KEEP];
static volatile bool     step_on = false;

static uint16_t step_code(uint32_t k)
{
    return (uint16_t)(STEP_LOW + ((k * 1237u) % STEP_SPAN));
}

void adc_ch0_event(uint16_t result)
{
    const uint32_t k = adc_events++;
    if (k < S2_KEEP) { adc_keep[k] = result; }
    if (step_on) { dac2_set(step_code(k + 1u)); }
}

static int32_t predict(uint16_t code)
{
    return (int32_t)(g_gain * (float)code + g_offs + 0.5f);
}

/* Sample `n` results at `rate` with the CPU reading each one (DMA down),
 * SCCP1 in timer mode. Returns the number of results. */
static uint32_t adc_collect(uint32_t n_ticks, uint32_t n)
{
    (void)capture_settle();                  /* DMA channel down          */
    adc_events = 0u;
    adc_ch0_irq(true, true);
    (void)sccp1_start(n_ticks, SCCP_CLK_GEN13, SCCP_MODE_TIMER, SCCP_EVENT_SPECIAL);
    const uint32_t limit = (uint32_t)(((uint64_t)n * n_ticks * TIMEBASE_HZ) / g_trig_hz) * 2u
                           + 10u * TICKS_PER_MS;
    const uint32_t t0 = timebase_ticks();
    while ((adc_events < n) && ((timebase_ticks() - t0) < limit)) { }
    sccp1_stop();
    wait_ticks(25u);
    adc_ch0_irq(false, false);
    return adc_events;
}

static uint32_t mean_kept(uint32_t n)
{
    uint32_t acc = 0u;
    if (n > S2_KEEP) { n = S2_KEEP; }
    if (n == 0u) { return 0u; }
    for (uint32_t i = 0; i < n; i++) { acc += adc_keep[i]; }
    return (acc + n / 2u) / n;
}

/* ------------------------------------------------------------------ *
 * The triangle evaluator
 *
 * 1. Coarse turning points with a hysteresis of a quarter of the swing.
 * 2. A straight line fitted to the inside of every segment between them
 *    (an eighth of the segment left out at each end: the first DAC step
 *    after a turn uses a scaled SLPDAT, p1421, and the ADC's settling
 *    rounds the corner).
 * 3. Refined turning points where neighbouring lines intersect - to a
 *    fraction of a sample, independent of DNL.
 * 4. Complete slopes = distance between refined turning points, reported
 *    per direction with their largest deviation (dev).
 * 5. THE GRID VERDICT, "slip": a lost sample shifts every later turning
 *    point by exactly one sample, a repeated one by minus one. The
 *    turning points at the ends of the slope the fault sits in move by
 *    about half of that each (the line through that slope is a
 *    compromise), so a single slope only comes out half a sample off -
 *    too close to the noise at short slopes. Four turning points that
 *    enclose the faulty slope carry the whole sample: (P[j+4] - P[j]) -
 *    2 * T, T the median period, is 0 on a clean grid and +-1 across a
 *    fault. With fewer than five turning points it falls back to one
 *    period, (P[j+2] - P[j]) - T, which sees only about half a fault.
 * 6. Where the step per sample is at least 40 LSB, every single step
 *    inside a segment: below half a step is a repeated sample, above one
 *    and a half steps a lost one. Not below 40: the DAC's DNL of +-5 LSB
 *    (Table 40-42) makes the difference of two neighbouring samples
 *    scatter by +-10 LSB plus noise, and at 14 LSB per sample a clean
 *    ramp already produced "repeated" and "lost" steps in the host test
 *    of this code (25.09.2026).
 *
 * The end segments are cut to the median slope length before their line
 * is fitted: the window can end shortly after a turning point the
 * hysteresis has not recognised yet, and a line through both sides of it
 * put the last real turning point 1.7 samples off in that host test.
 * ------------------------------------------------------------------ */
#define TP_MAX          160u
#define STEP_CHECK_LSB  40.0

typedef struct {
    uint32_t mn, mx, pp;
    uint32_t tps;                 /* coarse turning points found         */
    uint32_t n_up, n_dn;          /* complete rising / falling slopes    */
    float    l_up, l_dn;          /* their mean length in samples        */
    float    dev;                 /* largest deviation from the mean     */
    float    step;                /* mean |slope| in LSB per sample      */
    uint32_t zero, dbl;           /* repeated / lost sample steps        */
    float    slip;                /* the grid verdict's number, samples  */
    uint32_t slip_k;              /* 4 (two periods) or 2 (one period)   */
    uint32_t slip_n;              /* how many spans were compared        */
    bool     step_checked;
    bool     overflow;            /* more turning points than TP_MAX     */
} tri_t;

static uint16_t tp_idx[TP_MAX];
static double   per_tmp[TP_MAX];
static double   seg_a[TP_MAX + 1u], seg_b[TP_MAX + 1u];
static bool     seg_ok[TP_MAX + 1u];
static double   tp_pos[TP_MAX];

static bool fit_line(const volatile uint16_t *x, uint32_t a, uint32_t b,
                     double *slope, double *icpt)
{
    if ((b <= a) || ((b - a) < 5u)) { return false; }
    const double c = 0.5 * (double)(a + b);
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double n = (double)(b - a + 1u);
    for (uint32_t i = a; i <= b; i++) {
        const double u = (double)i - c;
        const double y = (double)x[i];
        sx += u; sy += y; sxx += u * u; sxy += u * y;
    }
    const double den = n * sxx - sx * sx;
    if (den == 0.0) { return false; }
    const double m = (n * sxy - sx * sy) / den;
    *slope = m;
    *icpt  = (sy - m * sx) / n - m * c;      /* y = m * i + icpt           */
    return true;
}

static void tri_eval(const volatile uint16_t *x, uint32_t n, tri_t *r)
{
    uint32_t mn = 0xFFFFu, mx = 0u;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] < mn) { mn = x[i]; }
        if (x[i] > mx) { mx = x[i]; }
    }
    r->mn = mn; r->mx = mx; r->pp = mx - mn;
    r->tps = 0u; r->n_up = 0u; r->n_dn = 0u;
    r->l_up = 0.0f; r->l_dn = 0.0f; r->dev = 0.0f; r->step = 0.0f;
    r->zero = 0u; r->dbl = 0u; r->step_checked = false; r->overflow = false;
    r->slip = 99.0f; r->slip_k = 0u; r->slip_n = 0u;
    if (n < 16u) { return; }

    /* 1. coarse turning points */
    uint32_t h = r->pp / 4u;
    if (h < 8u) { h = 8u; }
    int dir = 0;
    uint32_t hi_v = x[0], lo_v = x[0], hi_i = 0u, lo_i = 0u;
    uint32_t ext_v = x[0], ext_i = 0u;
    uint32_t ntp = 0u;
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = x[i];
        if (dir == 0) {
            if (v > hi_v) { hi_v = v; hi_i = i; }
            if (v < lo_v) { lo_v = v; lo_i = i; }
            if (v + h < hi_v) {                      /* now falling        */
                if ((hi_i > 2u) && (ntp < TP_MAX)) { tp_idx[ntp++] = (uint16_t)hi_i; }
                dir = -1; ext_v = v; ext_i = i;
            } else if (v > lo_v + h) {               /* now rising         */
                if ((lo_i > 2u) && (ntp < TP_MAX)) { tp_idx[ntp++] = (uint16_t)lo_i; }
                dir = 1; ext_v = v; ext_i = i;
            }
        } else if (dir > 0) {
            if (v >= ext_v) { ext_v = v; ext_i = i; }
            else if ((ext_v - v) > h) {
                if (ntp < TP_MAX) { tp_idx[ntp++] = (uint16_t)ext_i; } else { r->overflow = true; }
                dir = -1; ext_v = v; ext_i = i;
            }
        } else {
            if (v <= ext_v) { ext_v = v; ext_i = i; }
            else if ((v - ext_v) > h) {
                if (ntp < TP_MAX) { tp_idx[ntp++] = (uint16_t)ext_i; } else { r->overflow = true; }
                dir = 1; ext_v = v; ext_i = i;
            }
        }
    }
    r->tps = ntp;
    if (ntp < 2u) { return; }

    /* the median distance between coarse turning points, for the ends */
    uint32_t med = n;
    {
        uint32_t cnt = 0u;
        for (uint32_t j = 1; j < ntp; j++) { per_tmp[cnt++] = (double)(tp_idx[j] - tp_idx[j - 1u]); }
        for (uint32_t i = 1; i < cnt; i++) {                  /* insertion sort */
            const double v = per_tmp[i];
            uint32_t k = i;
            while ((k > 0u) && (per_tmp[k - 1u] > v)) { per_tmp[k] = per_tmp[k - 1u]; k--; }
            per_tmp[k] = v;
        }
        if (cnt != 0u) { med = (uint32_t)per_tmp[cnt / 2u]; }
    }

    /* 2. a line through the inside of every segment */
    double step_sum = 0.0;
    uint32_t step_n = 0u;
    for (uint32_t s = 0; s <= ntp; s++) {
        uint32_t a = (s == 0u) ? 0u : tp_idx[s - 1u];
        uint32_t b = (s == ntp) ? (n - 1u) : tp_idx[s];
        if ((s == 0u) && (b > med))           { a = b - med; }  /* cut the ends */
        if ((s == ntp) && ((b - a) > med))    { b = a + med; }
        const uint32_t len = b - a;
        uint32_t m = len / 8u;
        if (m < 2u) { m = 2u; }
        seg_ok[s] = (len > 2u * m + 5u) && fit_line(x, a + m, b - m, &seg_a[s], &seg_b[s]);
        if (seg_ok[s] && (s > 0u) && (s < ntp)) {      /* inner segments   */
            step_sum += (seg_a[s] >= 0.0) ? seg_a[s] : -seg_a[s];
            step_n++;
        }
    }
    r->step = (step_n != 0u) ? (float)(step_sum / (double)step_n) : 0.0f;

    /* 5. every step inside the inner segments, where it is large enough */
    if (r->step >= STEP_CHECK_LSB) {
        r->step_checked = true;
        for (uint32_t s = 1; s < ntp; s++) {
            if (!seg_ok[s]) { continue; }
            const uint32_t a = tp_idx[s - 1u], b = tp_idx[s];
            uint32_t m = (b - a) / 8u;
            if (m < 2u) { m = 2u; }
            const double sl = seg_a[s];
            const double as = (sl >= 0.0) ? sl : -sl;
            for (uint32_t i = a + m; i < b - m; i++) {
                const double d = ((double)x[i + 1u] - (double)x[i]) * ((sl >= 0.0) ? 1.0 : -1.0);
                if (d < 0.5 * as)      { r->zero++; }
                else if (d > 1.5 * as) { r->dbl++; }
            }
        }
    }

    /* 3. refined turning points: line s meets line s+1 */
    for (uint32_t j = 0; j < ntp; j++) {
        if (seg_ok[j] && seg_ok[j + 1u] && (seg_a[j] != seg_a[j + 1u])) {
            tp_pos[j] = (seg_b[j + 1u] - seg_b[j]) / (seg_a[j] - seg_a[j + 1u]);
        } else {
            tp_pos[j] = -1.0;
        }
    }

    /* 4. complete slopes: inner segments between two refined points */
    double up = 0.0, dn = 0.0;
    for (uint32_t s = 1; s < ntp; s++) {
        if ((tp_pos[s - 1u] < 0.0) || (tp_pos[s] < 0.0) || !seg_ok[s]) { continue; }
        const double L = tp_pos[s] - tp_pos[s - 1u];
        if (seg_a[s] > 0.0) { up += L; r->n_up++; } else { dn += L; r->n_dn++; }
    }
    if (r->n_up != 0u) { r->l_up = (float)(up / (double)r->n_up); }
    if (r->n_dn != 0u) { r->l_dn = (float)(dn / (double)r->n_dn); }
    double dev = 0.0;
    for (uint32_t s = 1; s < ntp; s++) {
        if ((tp_pos[s - 1u] < 0.0) || (tp_pos[s] < 0.0) || !seg_ok[s]) { continue; }
        const double L = tp_pos[s] - tp_pos[s - 1u];
        const double d = L - ((seg_a[s] > 0.0) ? (double)r->l_up : (double)r->l_dn);
        const double ad = (d >= 0.0) ? d : -d;
        if (ad > dev) { dev = ad; }
    }
    r->dev = (float)dev;

    /* 5. slip over two periods (or one, with few turning points). Only
     * turning points between two FULL slopes count, i.e. not the first
     * and the last: next to them lies a segment the window cuts, whose
     * line sees one rounded corner instead of two and is biased
     * differently - 0.7 samples at 40 MSPS with the DAC filter modelled,
     * a false "slip" on clean data (host test, 25.09.2026). */
    const uint32_t first = 1u, last = ntp - 2u;           /* inclusive     */
    const uint32_t inner = (ntp >= 3u) ? (last - first + 1u) : 0u;
    const uint32_t k = (inner >= 5u) ? 4u : 2u;
    uint32_t cnt = 0u;
    for (uint32_t j = first; (inner != 0u) && ((j + 2u) <= last); j++) {   /* periods -> median T */
        if ((tp_pos[j] >= 0.0) && (tp_pos[j + 2u] >= 0.0)) { per_tmp[cnt++] = tp_pos[j + 2u] - tp_pos[j]; }
    }
    if (cnt == 0u) { return; }
    for (uint32_t i = 1; i < cnt; i++) {
        const double v = per_tmp[i];
        uint32_t q = i;
        while ((q > 0u) && (per_tmp[q - 1u] > v)) { per_tmp[q] = per_tmp[q - 1u]; q--; }
        per_tmp[q] = v;
    }
    const double T = per_tmp[cnt / 2u];
    double worst = 0.0;
    uint32_t spans = 0u;
    for (uint32_t j = first; (j + k) <= last; j++) {
        if ((tp_pos[j] < 0.0) || (tp_pos[j + k] < 0.0)) { continue; }
        const double e = (tp_pos[j + k] - tp_pos[j]) - (double)(k / 2u) * T;
        const double ae = (e >= 0.0) ? e : -e;
        if (ae > worst) { worst = ae; }
        spans++;
    }
    if (spans != 0u) { r->slip = (float)worst; r->slip_k = k; r->slip_n = spans; }
}

/* The grid verdict: slopes of both directions seen, no turning point
 * half a sample or more off the grid over two periods, no repeated or
 * lost step where steps are checked. */
#define GRID_SLIP_MAX   0.5f
static bool tri_grid_ok(const tri_t *r)
{
    return (r->n_up >= 1u) && (r->n_dn >= 1u) && !r->overflow && (r->slip_n != 0u) &&
           (r->slip < GRID_SLIP_MAX) && (r->zero == 0u) && (r->dbl == 0u);
}

static void ln_tri(const tri_t *r)
{
    ln_u("pp", r->pp); ln_u("min", r->mn); ln_u("max", r->mx);
    ln_u("tp", r->tps);
    ln_u("up", r->n_up); ln_f("Lup_x100", r->l_up, 100u);
    ln_u("dn", r->n_dn); ln_f("Ldn_x100", r->l_dn, 100u);
    ln_f("dev_x100", r->dev, 100u);
    ln_f("slip_x100", r->slip, 100u); ln_u("slip_k", r->slip_k); ln_u("slip_n", r->slip_n);
    ln_f("step_x10", r->step, 10u);
    if (r->step_checked) { ln_u("zero", r->zero); ln_u("dbl", r->dbl); }
    if (r->overflow) { ln_s(" overflow=1"); }
}

/* A window as @DUMP lines: 64 samples per line, 3 hex digits each. */
static void dump_window(uint32_t n_tag, const volatile uint16_t *x, uint32_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[200];                          /* 6 + 64 * 3 + 2 + 1 = 201  */
    ln_len = 0u;
    ln_s("@DUMP S");
    char t[12];
    fmt_u(t, g_stage); ln_s(t); ln_s("."); fmt_u(t, n_tag); ln_s(t);
    ln_u("n", n);
    ln_s("\r\n");
    ln[ln_len] = '\0';
    console_puts(ln);
    for (uint32_t i = 0; i < n; i += 64u) {
        uint32_t p = 0u;
        line[p++] = '@'; line[p++] = 'D'; line[p++] = ' ';
        for (uint32_t k = i; (k < i + 64u) && (k < n); k++) {
            const uint32_t v = x[k] & 0xFFFu;
            line[p++] = hex[(v >> 8) & 0xFu];
            line[p++] = hex[(v >> 4) & 0xFu];
            line[p++] = hex[v & 0xFu];
        }
        line[p++] = '\r'; line[p++] = '\n'; line[p] = '\0';
        console_puts(line);
    }
}

/* ------------------------------------------------------------------ *
 * Set-up and restore
 * ------------------------------------------------------------------ */
static void mark(uint32_t stage)
{
    g_stage = stage;
    chain_mark = CHAIN_MARK_MAGIC | stage;
}

/* Clock tree for the chain, core 5 in Single mode, DMA down. Quiet; S0
 * reports what came out. */
static uint32_t setup_rc_pll;
static bool     setup_trig, setup_dac;

static bool setup(void)
{
    timebase_init();
    (void)capture_settle();
    sccp1_stop();
    (void)capture_set_half_len(SAMPLES_PER_HALF_MAX);
    setup_rc_pll = capture_set_pll(5u, 1u);           /* 320 MHz, VCO 1600 */
    setup_trig   = clock_trig_on();                   /* CLKGEN13 160 MHz  */
    clock_dac_select(CLOCK_DAC_PLL1_VCO);
    setup_dac    = dac2_level_start(0x800u);          /* CLKGEN7 400 MHz   */
    (void)capture_select_core(CHAIN_CORE, CHAIN_PINSEL, CHAIN_SAMC);
    adc_set_mode_single(SCCP1_ADC_TRIGGER);
    adc_set_irqsel(0u);
    (void)capture_settle();                           /* DMA down          */
    if (g_trig_hz == 0u) { g_trig_hz = TRIG_HZ_NOMINAL; }
    g_setup_ok = (setup_rc_pll == CLKDIV_OK) && setup_trig && setup_dac;
    return g_setup_ok;
}

static void restore(void)
{
    (void)capture_settle();
    sccp1_stop();
    sccp1_count(false);
    adc_ch0_irq(false, false);
    step_on = false;
    dac2_off();
    clock_dac_select(CLOCK_DAC_PLL1_VCO);
    (void)capture_select_core(ADC_INSTANCE, ADC_PINSEL, ADC_SAMC);  /* burst mode again */
    (void)capture_set_pll(ADC_PLL_POSTDIV1, ADC_PLL_POSTDIV2);
    counters_clear();
}

/* Pick SLPDAT so that one slope lasts about SLOPE_TARGET samples at this
 * rate, with the widest range the limits allow: DACLOW >= 0xCD + SLPDAT
 * and DACDAT <= 0xF32 - SLPDAT (note 1 of Example 18-3, p1422), 32 codes
 * of margin inside that. The slope in samples is
 *   span * 32 * rate / (SLPDAT * F_DAC),   span = 0xE65 - 2 * SLPDAT - 64,
 * which falls as SLPDAT rises, so the first SLPDAT at or below the target
 * is taken. 128 samples: short enough for about 16 turning points per
 * window, so that a fault almost anywhere in it is enclosed by four of
 * them (see tri_eval), and steep enough (about 27 LSB per sample) for a
 * turning point to a few hundredths of a sample; long enough that the
 * corners the DAC's output filter rounds (600 ns, Table 40-43) stay in
 * the eighth of each slope the fit leaves out, even at 40 MSPS. At
 * 100 kSPS the slowest triangle the DAC makes lasts only about 29
 * samples per slope; that is what it gets there. */
#define SLOPE_TARGET  128u
static bool triangle_for(uint32_t rate, uint16_t *slp_out)
{
    const uint32_t f = clock_dac_hz();
    if (f == 0u) { return false; }
    const uint32_t full = DAC_CODE_MAX - DAC_CODE_MIN - 64u;       /* 3621 */
    uint32_t s = 1u;
    for (; (2u * s + 64u) < full; s++) {
        const uint64_t span = full - 2u * s;
        const uint64_t samples = span * 32u * rate / ((uint64_t)s * f);
        if (samples <= SLOPE_TARGET) { break; }
    }
    *slp_out = (uint16_t)s;
    return dac2_triangle_start((uint16_t)(DAC_CODE_MIN + s + 32u),
                               (uint16_t)(DAC_CODE_MAX - s - 32u), (uint16_t)s);
}

/* One contiguous window from a running triggered stream: three blocks,
 * the DMA ISR stops the trigger at the third DONE. At a high rate a few
 * more samples arrive before the trigger is off and overwrite the start
 * of the buffer with newer ones; *pos says where the contiguous window
 * of the last block begins (0 at low rates). */
static bool grab_window(uint32_t n_ticks, bool data_src, uint32_t *pos)
{
    const uint32_t block = 2u * capture_half_len();
    if (!capture_chain_start(n_ticks, SCCP_MODE_TIMER, 3u, data_src)) { return false; }
    const uint32_t limit = (uint32_t)(((uint64_t)3u * block * n_ticks * TIMEBASE_HZ) / g_trig_hz) * 2u
                           + 20u * TICKS_PER_MS;
    const uint32_t rc = capture_chain_wait(limit);
    const uint64_t x  = capture_chain_stop();
    if (rc != 0u) { return false; }
    const uint64_t extra = (x > 3ull * block) ? (x - 3ull * block) : 0u;
    *pos = (extra < block) ? (uint32_t)extra : block;
    return *pos < block - 64u;
}

/* ------------------------------------------------------------------ *
 * S0 - preconditions
 * ------------------------------------------------------------------ */
static void cm_line(uint32_t n, const char *name, uint32_t sel, uint32_t expect)
{
    const uint32_t hz = clock_monitor_hz(sel);
    ln_begin(n);
    ln_s(" cm="); ln_s(name);
    ln_u("hz", hz); ln_u("expect", expect);
    if (hz == 0u) { ln_s(" note=no_count"); ln_end(V_INFO); return; }
    const uint32_t d = (hz > expect) ? (hz - expect) : (expect - hz);
    ln_end((d <= expect / 200u) ? V_PASS : V_FAIL);          /* 0.5 %       */
}

static void stage0(void)
{
    mark(0u);
    ln_begin(1u); ln_s(" build=" BUILD_ID); ln_end(V_INFO);

    const uint32_t t1 = timebase_check();
    ln_begin(2u); ln_u("timer1_per_100ms", t1); ln_u("expect", 1250000u);
    ln_end(((t1 >= 1249875u) && (t1 <= 1250125u)) ? V_PASS : V_FAIL);   /* 0.01 % */

    ln_begin(3u); ln_u("pll_rc", setup_rc_pll); ln_u("clkgen13_on", setup_trig ? 1u : 0u);
    ln_u("dac_on", setup_dac ? 1u : 0u);
    ln_u("adc_hz", clock_adc_hz()); ln_u("trig_hz", clock_trig_hz()); ln_u("dac_hz", clock_dac_hz());
    ln_x("VCO1DIV", VCO1DIV); ln_x("CLK13DIV", CLK13DIV); ln_x("CLK7CON", CLK7CON);
    ln_end(g_setup_ok ? V_PASS : V_FAIL);

    cm_line(4u, "pll1_out", CM_PLL1_OUT, 320000000u);
    cm_line(5u, "pll1_vcodiv", CM_PLL1_VCODIV, 400000000u);
    cm_line(6u, "clkgen6_adc", CM_CLKGEN6, 320000000u);
    cm_line(7u, "clkgen7_dac", CM_CLKGEN7, 400000000u);
    cm_line(8u, "pll2_vcodiv", CM_PLL2_VCODIV, 500000000u);

    const uint32_t cal = adc_cal_bits(), other = adc_other_channels_armed();
    ln_begin(9u); ln_u("core", adc_core()); ln_u("pinsel", adc_pinsel()); ln_u("samc", adc_samc());
    ln_u("mode", adc_mode()); ln_u("trg1src", adc_trg1()); ln_x("cal", cal); ln_u("other_channels", other);
    ln_end(((cal == 0u) && (other == 0u) && (adc_core() == CHAIN_CORE)) ? V_PASS : V_FAIL);

    const uint32_t an = (ANSELA >> 8) & 1u, tr = (TRISA >> 8) & 1u;
    ln_begin(10u); ln_u("RA8_ansel", an); ln_u("RA8_tris", tr);
    ln_end(((an == 1u) && (tr == 1u)) ? V_PASS : V_FAIL);

    ln_begin(11u); ln_u("ip_dma0", IPC9bits.DMA0IP); ln_u("ip_u2rx", IPC12bits.U2RXIP);
    ln_u("ip_cct1", IPC6bits.CCT1IP); ln_u("ip_ad5ch0", IPC30bits.AD5CH0IP);
    ln_s(" note=counting_irqs_get_3_when_enabled");
    ln_end(V_INFO);
}

/* ------------------------------------------------------------------ *
 * S1 - SCCP1 alone
 * ------------------------------------------------------------------ */
static void stage1(void)
{
    mark(1u);
    (void)capture_settle();
    /* Its clock: CCP1TMR across 10 ms of Timer1, period as long as it
     * gets so that the counter does not roll over inside the window. */
    (void)sccp1_start(0xFFFFFFFFu, SCCP_CLK_GEN13, SCCP_MODE_TIMER, SCCP_EVENT_SPECIAL);
    const uint32_t c0 = sccp1_tmr(), t0 = timebase_ticks();
    wait_ticks(10u * TICKS_PER_MS);
    const uint32_t c1 = sccp1_tmr(), t1 = timebase_ticks();
    sccp1_stop();
    const uint32_t hz = (uint32_t)(((uint64_t)(c1 - c0) * TIMEBASE_HZ) / (t1 - t0));
    const uint32_t d = (hz > TRIG_HZ_NOMINAL) ? (hz - TRIG_HZ_NOMINAL) : (TRIG_HZ_NOMINAL - hz);
    ln_begin(1u); ln_u("sccp_hz", hz); ln_u("expect", TRIG_HZ_NOMINAL);
    ln_u("in_spec_max", 200000000u);
    const bool ok = d <= TRIG_HZ_NOMINAL / 200u;
    ln_end(ok ? V_PASS : V_FAIL);
    /* From here on the rates are computed from the clock as measured,
     * whatever it is - a divider that did not divide shows as a wrong
     * clock here and not as a wrong rate everywhere else. */
    if (hz > 1000000u) { g_trig_hz = hz; }

    /* Its period: interrupts over 100 ms at 1, 10 and 100 kHz, in timer
     * and in output-compare mode. */
    static const uint32_t rates[] = { 1000u, 10000u, 100000u };
    uint32_t n = 2u;
    for (uint32_t m = 0; m < 2u; m++) {
        for (uint32_t r = 0; r < 3u; r++) {
            const uint32_t ticks = g_trig_hz / rates[r];
            sccp1_count(true);
            (void)sccp1_start(ticks, SCCP_CLK_GEN13, (m == 0u) ? SCCP_MODE_TIMER : SCCP_MODE_OC,
                              SCCP_EVENT_SPECIAL);
            const uint32_t s0 = timebase_ticks();
            wait_ticks(100u * TICKS_PER_MS);
            sccp1_stop();
            const uint32_t w = timebase_ticks() - s0;
            const uint32_t ev_t = sccp1_timer_events, ev_c = sccp1_cmp_events;
            sccp1_count(false);
            const uint32_t exp = (uint32_t)expected_triggers(w, ticks);
            const uint32_t got = (m == 0u) ? ev_t : ev_c;
            ln_begin(n++);
            ln_s((m == 0u) ? " mode=timer" : " mode=oc");
            ln_u("hz", rates[r]); ln_u("cct1", ev_t); ln_u("ccp1", ev_c); ln_u("expect", exp);
            ln_end((absdiff64(got, exp) <= 2u) ? V_PASS : V_FAIL);
        }
    }
}

/* ------------------------------------------------------------------ *
 * S2 - SCCP1 -> ADC at low rates, the CPU counting both ends
 * ------------------------------------------------------------------ */
#define S2_LEVELS  8u
static void stage2(void)
{
    mark(2u);
    const uint32_t t100k = g_trig_hz / 100000u;

    /* The static transfer DAC -> RA8 -> ADC, 8 levels, 32 samples each at
     * 100 kHz, SAMC 0. A least-squares line gives the prediction every
     * later CPU-stepped check uses. */
    float sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
    uint32_t means[S2_LEVELS];
    ln_begin(1u); ln_s(" route=ra8 samc=0 levels");
    for (uint32_t l = 0; l < S2_LEVELS; l++) {
        const uint16_t code = (uint16_t)(0x100u + l * 0x1C0u);    /* 0x100..0xD40 */
        (void)dac2_level_start(code);
        wait_ticks(TICKS_PER_MS / 10u);
        const uint32_t got = adc_collect(t100k, 32u);
        means[l] = mean_kept(got);
        char t[12];
        fmt_u(t, means[l]); ln_s(l ? "," : "="); ln_s(t);
        sx += (float)code; sy += (float)means[l];
        sxx += (float)code * (float)code; sxy += (float)code * (float)means[l];
    }
    const float nl = (float)S2_LEVELS;
    const float den = nl * sxx - sx * sx;
    if (den != 0.0f) {
        g_gain = (nl * sxy - sx * sy) / den;
        g_offs = (sy - g_gain * sx) / nl;
    }
    uint32_t maxdev = 0u;
    for (uint32_t l = 0; l < S2_LEVELS; l++) {
        const int32_t e = (int32_t)means[l] - predict((uint16_t)(0x100u + l * 0x1C0u));
        const uint32_t ae = (uint32_t)((e < 0) ? -e : e);
        if (ae > maxdev) { maxdev = ae; }
    }
    g_tol = 3u * maxdev + 40u;
    ln_f("gain_x1000", g_gain, 1000u); ln_f("offs_x10", g_offs, 10u);
    ln_u("maxdev", maxdev); ln_u("tol", g_tol);
    /* A frozen or disconnected input gives a gain far from 1. */
    ln_end(((g_gain > 0.8f) && (g_gain < 1.2f)) ? V_PASS : V_FAIL);

    /* The same mid level with the longest sample time, and the two ends
     * over UREF: what the touch-pad network on RA8 does (C.11, C.10.8). */
    (void)dac2_level_start(0x800u);
    wait_ticks(TICKS_PER_MS / 10u);
    const uint32_t m0 = mean_kept(adc_collect(t100k, 32u));
    (void)capture_settle();
    (void)capture_set_input(CHAIN_PINSEL, 31u);
    const uint32_t m31 = mean_kept(adc_collect(t100k, 32u));
    (void)capture_set_input(CHAIN_PINSEL, CHAIN_SAMC);
    ln_begin(2u); ln_u("code", 0x800u); ln_u("samc0", m0); ln_u("samc31", m31);
    ln_end(V_INFO);

    (void)uref_route_dac2(false);
    (void)capture_set_input(DAC_UREF_PINSEL, CHAIN_SAMC);
    (void)dac2_level_start(0x200u); wait_ticks(TICKS_PER_MS / 10u);
    const uint32_t u_lo = mean_kept(adc_collect(t100k, 32u));
    (void)dac2_level_start(0xE00u); wait_ticks(TICKS_PER_MS / 10u);
    const uint32_t u_hi = mean_kept(adc_collect(t100k, 32u));
    (void)capture_set_input(CHAIN_PINSEL, CHAIN_SAMC);
    uref_off();
    ln_begin(3u); ln_s(" route=uref"); ln_u("lo_0x200", u_lo); ln_u("hi_0xE00", u_hi);
    ln_u("ra8_lo_pred", (uint32_t)predict(0x200u)); ln_u("ra8_hi_pred", (uint32_t)predict(0xE00u));
    ln_end(V_INFO);

    /* SCCP1 events against ADC results, 100 ms each. */
    static const uint32_t rates[] = { 1000u, 10000u, 100000u };
    uint32_t n = 4u;
    bool trig_ok = true;
    (void)dac2_level_start(0x800u);
    for (uint32_t k = 0; k < 4u; k++) {
        const bool oc = (k == 3u);
        const uint32_t hz = oc ? 10000u : rates[k];
        const uint32_t ticks = g_trig_hz / hz;
        (void)capture_settle();
        adc_events = 0u;
        sccp1_count(true);
        adc_ch0_irq(true, false);
        (void)sccp1_start(ticks, SCCP_CLK_GEN13, oc ? SCCP_MODE_OC : SCCP_MODE_TIMER, SCCP_EVENT_SPECIAL);
        wait_ticks(100u * TICKS_PER_MS);
        sccp1_stop();
        wait_ticks(25u);
        const uint32_t ev = oc ? sccp1_cmp_events : sccp1_timer_events;
        const uint32_t res = adc_events;
        adc_ch0_irq(false, false);
        sccp1_count(false);
        ln_begin(n++);
        ln_s(oc ? " mode=oc" : " mode=timer");
        ln_u("hz", hz); ln_u("sccp_events", ev); ln_u("adc_results", res);
        const bool ok = (ev > 0u) && (absdiff64(ev, res) <= 1u);
        if (!oc) { trig_ok = trig_ok && ok; }
        ln_end(ok ? V_PASS : V_FAIL);
    }
    g_trigger_ok = trig_ok;

    /* The CPU-stepped DAC: every sample must read the code the handler
     * wrote after the sample before. 256 samples at 100 kHz. */
    (void)dac2_level_start(step_code(0u));
    wait_ticks(TICKS_PER_MS / 10u);
    step_on = true;
    const uint32_t got = adc_collect(t100k, S2_KEEP);
    step_on = false;
    uint32_t bad = 0u, maxerr = 0u;
    for (uint32_t k = 0; (k < got) && (k < S2_KEEP); k++) {
        const int32_t e = (int32_t)adc_keep[k] - predict(step_code(k));
        const uint32_t ae = (uint32_t)((e < 0) ? -e : e);
        if (ae > maxerr) { maxerr = ae; }
        if (ae > g_tol) { bad++; }
    }
    ln_begin(8u); ln_s(" stepped"); ln_u("hz", 100000u); ln_u("samples", got);
    ln_u("bad", bad); ln_u("maxerr", maxerr); ln_u("tol", g_tol);
    ln_end(((got >= S2_KEEP) && (bad == 0u)) ? V_PASS : V_FAIL);
}

/* ------------------------------------------------------------------ *
 * S3 - ADC -> DMA -> buffer at 100 kHz, the DAC stepped by the CPU
 * ------------------------------------------------------------------ */
static void stage3(void)
{
    mark(3u);
    const uint32_t ticks = g_trig_hz / 100000u;
    const uint32_t block = 2u * capture_half_len();
    (void)dac2_level_start(step_code(0u));
    wait_ticks(TICKS_PER_MS / 10u);
    adc_events = 0u;
    step_on = true;
    adc_ch0_irq(true, false);                /* stepping only, no read     */
    const bool started = capture_chain_start(ticks, SCCP_MODE_TIMER, 3u, false);
    const uint32_t rc = started ? capture_chain_wait(200u * TICKS_PER_MS) : 6u;
    const uint64_t x = capture_chain_stop();
    adc_ch0_irq(false, false);
    step_on = false;

    ln_begin(1u); ln_u("hz", 100000u); ln_u("rc", rc);
    ln_u("xfer", (uint32_t)x); ln_u("adc_results", adc_events); ln_u("expect", 3u * block);
    ln_u("isr", isr_entries); ln_u("half", half_events); ln_u("done", done_events);
    ln_u("overrun", dma_overrun); ln_u("addr_err", dma_addr_err);
    const bool guard = capture_guard_ok();
    ln_u("guard_ok", guard ? 1u : 0u);
    const bool ok1 = (rc == 0u) && (x == 3u * block) && (adc_events == 3u * block) &&
                     (half_events == 3u) && (done_events == 3u) &&
                     (isr_entries == 6u) && (dma_overrun == 0u) && guard;
    ln_end(ok1 ? V_PASS : V_FAIL);

    /* buf[i] holds sample 2 * block + i: the third block, written from
     * buf[0] again after the channel reloaded by itself at the second
     * DONE. So every index checks fetch, store, order and the restart. */
    const volatile uint16_t *b = capture_buffer();
    uint32_t bad = 0u, first_bad = 0xFFFFu;
    for (uint32_t i = 0; i < block; i++) {
        const int32_t e = (int32_t)b[i] - predict(step_code(2u * block + i));
        const uint32_t ae = (uint32_t)((e < 0) ? -e : e);
        if (ae > g_tol) { if (bad == 0u) { first_bad = i; } bad++; }
    }
    ln_begin(2u); ln_s(" buffer_vs_stepped_codes"); ln_u("n", block); ln_u("bad", bad);
    if (bad != 0u) { ln_u("first_bad", first_bad); }
    ln_end(((rc == 0u) && (bad == 0u)) ? V_PASS : V_FAIL);
    if ((rc == 0u) && (bad != 0u)) { dump_window(2u, b, block); }
}

/* ------------------------------------------------------------------ *
 * S4 - every rate: triggers against transfers
 * ------------------------------------------------------------------ */
static bool count_at(uint32_t n_ticks, bool data_src, uint32_t tag)
{
    adc_set_irqsel(data_src ? 1u : 0u);
    const bool started = capture_chain_start(n_ticks, SCCP_MODE_TIMER, 0u, data_src);
    wait_ticks(50u * TICKS_PER_MS);
    const uint64_t x = capture_chain_stop();
    adc_set_irqsel(0u);
    const uint32_t w = capture_chain_window_ticks();
    const uint64_t e = expected_triggers(w, n_ticks);
    const uint32_t tol = trigger_tol(n_ticks);
    /* Busy time of one conversion: (2 * SAMC + 0.5) TAD sampling plus
     * 2 TAD converting (16.4.3) at TAD = 12.5 ns, against the period. */
    const uint32_t busy_ns = (uint32_t)((2u * CHAIN_SAMC + 2u) * 125u + 62u) / 10u;
    const uint32_t per_ns  = (uint32_t)((uint64_t)n_ticks * 1000000000ull / g_trig_hz);
    ln_begin(tag);
    ln_u("ksps", ksps_of(n_ticks)); ln_s(data_src ? " src=data irqsel=1" : " src=res irqsel=0");
    ln_u("xfer", (uint32_t)x); ln_u("expect", (uint32_t)e); ln_u("tol", tol);
    ln_u("overrun", dma_overrun); ln_u("brake", capture_overrun_aborted() ? 1u : 0u);
    ln_u("busy_ns", busy_ns); ln_u("period_ns", per_ns);
    const bool ok = started && (absdiff64(x, e) <= tol) && (dma_overrun == 0u) &&
                    !capture_overrun_aborted() && (dma_addr_err == 0u);
    ln_end(ok ? V_PASS : V_FAIL);
    return ok;
}

static void stage4(void)
{
    mark(4u);
    s4_run = true;
    (void)dac2_level_start(0x800u);
    for (uint32_t i = 0; i < LADDER_LEN; i++) {
        s4_ok[i] = count_at(ladder_n[i], false, 1u + 2u * i);
        s4_data[i] = false;
        if (!s4_ok[i]) {
            /* The variant that does not hang the DMA trigger on RES's
             * ready flag (ANALYSIS.md C.10 point 2). */
            s4_data[i] = count_at(ladder_n[i], true, 2u + 2u * i);
            s4_ok[i] = s4_data[i];
        }
    }
}

/* ------------------------------------------------------------------ *
 * S5 - the grid in the data
 * ------------------------------------------------------------------ */
static bool grid_at(uint32_t n_ticks, bool data_src, uint32_t tag)
{
    uint16_t slp = 0u;
    const uint32_t rate = rate_hz(n_ticks);
    if (data_src) { adc_set_irqsel(1u); }
    const bool dac_ok = triangle_for(rate, &slp);
    wait_ticks(TICKS_PER_MS);
    uint32_t pos = 0u;
    const bool got = dac_ok && grab_window(n_ticks, data_src, &pos);
    adc_set_irqsel(0u);
    const uint32_t block = 2u * capture_half_len();
    tri_t r;
    ln_begin(tag);
    ln_u("ksps", ksps_of(n_ticks)); ln_u("slpdat", slp); ln_u("dac_hz", clock_dac_hz());
    if (!got) {
        ln_s(dac_ok ? " note=no_window" : " note=dac_refused");
        ln_end(V_FAIL);
        return false;
    }
    const volatile uint16_t *b = capture_buffer() + pos;
    tri_eval(b, block - pos, &r);
    ln_u("from", pos);
    ln_tri(&r);
    const uint32_t model = dac2_slope_samples_x1000(rate);
    const float mean = (r.l_up + r.l_dn) * 0.5f;
    ln_u("model_x100", (model + 5u) / 10u);
    if (model != 0u) { ln_f("ratio_x1000", mean * 1000.0f / (float)model, 1000u); }
    const bool ok = tri_grid_ok(&r);
    ln_end(ok ? V_PASS : V_FAIL);
    if (!ok) { dump_window(tag, capture_buffer(), block); }
    return ok;
}

static void stage5(void)
{
    mark(5u);
    s5_run = true;
    for (uint32_t i = 0; i < LADDER_LEN; i++) {
        s5_ok[i] = grid_at(ladder_n[i], s4_data[i], 1u + i);
    }
    /* The DAC on the second VCO (PLL2, 500 MHz) at 8 MSPS: the slope
     * lengths must follow the model's 4/5 - a check of the model rather
     * than of the grid, and of whether the grid verdict depends on the
     * DAC being coherent with the sample clock. */
    clock_dac_select(CLOCK_DAC_PLL2_VCO);
    ln_begin(20u); ln_s(" dac=pll2_vcodiv"); ln_end(V_INFO);
    (void)grid_at(ladder_n[IDX_8MSPS], s4_data[IDX_8MSPS], 21u);
    clock_dac_select(CLOCK_DAC_PLL1_VCO);
    (void)dac2_level_start(0x800u);
}

/* ------------------------------------------------------------------ *
 * S6 / S9 - the stream with the CPU processing every half
 * ------------------------------------------------------------------ */
#define SNAP_MAX  60u
typedef struct {
    uint32_t t_ms, halves, overrun, late, missed;
    uint64_t xfer;
} snap_t;
static snap_t snaps[SNAP_MAX];

/* Run the stream for `ms`, the CPU in capture_service() throughout, as
 * main() does. Snapshots every `snap_ms` (0 = none). Nothing is printed
 * while it runs. Returns the verdict and prints its line. With `recipe`
 * and a PASS, the chain's registers are printed while it still runs. */
static bool stream_for(uint32_t n_ticks, bool data_src, uint32_t ms, uint32_t snap_ms,
                       uint32_t tag, bool recipe)
{
    adc_set_irqsel(data_src ? 1u : 0u);
    const bool started = capture_chain_start(n_ticks, SCCP_MODE_TIMER, 0u, data_src);
    uint32_t nsnap = 0u;
    const uint32_t t0 = timebase_ticks();
    uint32_t next = snap_ms * TICKS_PER_MS;
    const uint32_t end = ms * TICKS_PER_MS;
    while (started) {
        (void)capture_service();
        const uint32_t el = timebase_ticks() - t0;
        if ((snap_ms != 0u) && (el >= next) && (nsnap < SNAP_MAX)) {
            snaps[nsnap].t_ms = el / TICKS_PER_MS;
            snaps[nsnap].halves = blocks_done;
            snaps[nsnap].overrun = dma_overrun;
            snaps[nsnap].late = late_service;
            snaps[nsnap].missed = proc_missed;
            snaps[nsnap].xfer = capture_transfers();
            nsnap++;
            next += snap_ms * TICKS_PER_MS;
        }
        if (el >= end) { break; }
        if (!capture_chain_active()) { break; }   /* the brake fired     */
    }
    /* The verdict's numbers, taken before anything is printed. */
    const uint32_t ov = dma_overrun, la = late_service, mi = proc_missed;
    const uint32_t isr = isr_entries, hf = half_events, dn = done_events;
    const uint32_t pmax = proc_ticks_max, pcnt = proc_count;
    const uint32_t pmean = (pcnt != 0u) ? (proc_ticks_sum / pcnt) : 0u;
    const bool brake = capture_overrun_aborted();
    const bool guard = capture_guard_ok();
    const bool clean_so_far = started && (ov == 0u) && (la == 0u) && (mi == 0u) && !brake;
    if (recipe && clean_so_far) {
        say("@RECIPE the chain's registers while it runs\r\n");
        clock_regs_dump();
        sccp1_regs_dump();
        adc_regs_dump();
        dma0_regs_dump();
        dac_regs_dump();
        console_kv_hex("IPC9 (DMA0IP 22:20)", IPC9);
        console_kv_hex("IPC12 (U2RXIP 26:24)", IPC12);
        say("@RECIPE end\r\n");
    }
    const uint64_t x = capture_chain_stop();
    adc_set_irqsel(0u);
    const uint32_t w = capture_chain_window_ticks();
    const uint64_t e = expected_triggers(w, n_ticks);
    const uint32_t tol = trigger_tol(n_ticks);

    for (uint32_t k = 0; k < nsnap; k++) {
        ln_begin(tag); ln_u("t_ms", snaps[k].t_ms); ln_u("halves", snaps[k].halves);
        ln_u("overrun", snaps[k].overrun); ln_u("late", snaps[k].late);
        ln_u("missed", snaps[k].missed); ln_u("xfer_M", (uint32_t)(snaps[k].xfer / 1000000u));
        ln_end(V_INFO);
    }
    /* The budget: one half lasts half_len * N / f_trig; in Timer1 ticks
     * and CPU cycles (16 per tick). */
    const uint32_t hl = capture_half_len();
    const uint32_t half_ticks = (uint32_t)(((uint64_t)hl * n_ticks * TIMEBASE_HZ) / g_trig_hz);
    const uint32_t load_x10 = (half_ticks != 0u) ? (pmax * 1000u / half_ticks) : 0u;
    const int32_t  free_cyc = (int32_t)(((int32_t)half_ticks - (int32_t)pmean) * (int32_t)CPU_PER_TICK) / (int32_t)hl;
    ln_begin(tag);
    ln_u("ksps", ksps_of(n_ticks)); ln_u("ms", w / TICKS_PER_MS);
    ln_s(data_src ? " src=data" : " src=res");
    ln_u("xfer", (uint32_t)(x > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)x));
    ln_u("expect", (uint32_t)(e > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)e)); ln_u("tol", tol);
    ln_u("overrun", ov); ln_u("late", la); ln_u("missed", mi);
    ln_u("isr", isr); ln_u("half", hf); ln_u("done", dn);
    ln_u("brake", brake ? 1u : 0u); ln_u("guard_ok", guard ? 1u : 0u);
    ln_u("load_max_x10", load_x10); ln_i("free_cyc_per_sample", free_cyc);
    const bool ok = clean_so_far && guard && (absdiff64(x, e) <= tol) && (isr == hf + dn);
    ln_end(ok ? V_PASS : V_FAIL);
    return ok;
}

static void stage6(void)
{
    mark(6u);
    s6_run = true;
    (void)dac2_level_start(0x800u);
    for (uint32_t i = 0; i < LADDER_LEN; i++) {
        s6_ok[i] = false;
        if (s4_run && !s4_ok[i]) {
            ln_begin(1u + i); ln_u("ksps", ksps_of(ladder_n[i])); ln_s(" note=failed_S4");
            ln_end(V_SKIP);
            continue;
        }
        s6_ok[i] = stream_for(ladder_n[i], s4_data[i], 1000u, 0u, 1u + i, false);
    }
}

/* ------------------------------------------------------------------ *
 * S7 - start, stop, restart, rate change
 * ------------------------------------------------------------------ */
#define SENTINEL  0xFFFFu                   /* no 12-bit result looks so   */
static bool start_check(uint32_t tag)
{
    const uint32_t n_ticks = ladder_n[IDX_1MSPS];
    const uint32_t block = 2u * capture_half_len();
    (void)capture_settle();
    capture_fill(SENTINEL);
    const bool started = capture_chain_start(n_ticks, SCCP_MODE_TIMER, 0u, false);
    wait_ticks(100u * TIMEBASE_HZ / 1000000u);          /* 100 us          */
    const uint64_t x = capture_chain_stop();
    const volatile uint16_t *b = capture_buffer();
    const uint32_t k = (x < block) ? (uint32_t)x : block;
    uint32_t hole = 0u, extra = 0u;
    for (uint32_t i = 0; i < block; i++) {
        if ((i < k) && (b[i] == SENTINEL))  { hole++; }
        if ((i >= k) && (b[i] != SENTINEL)) { extra++; }
    }
    ln_begin(tag); ln_u("ksps", ksps_of(n_ticks)); ln_u("xfer", (uint32_t)x);
    ln_u("written_holes", hole); ln_u("written_beyond", extra);
    const bool ok = started && (k > 50u) && (k < 200u) && (hole == 0u) && (extra == 0u);
    ln_end(ok ? V_PASS : V_FAIL);
    return ok;
}

static uint32_t buf_sum(void)
{
    const volatile uint16_t *b = capture_buffer();
    uint32_t s = 0u;
    for (uint32_t i = 0; i < 2u * capture_half_len(); i++) { s = (s * 31u) + b[i]; }
    return s;
}

static void stage7(void)
{
    mark(7u);
    (void)dac2_level_start(0x800u);
    (void)start_check(1u);

    const uint32_t s0 = buf_sum();
    wait_ticks(10u * TICKS_PER_MS);
    const uint32_t s1 = buf_sum();
    ln_begin(2u); ln_s(" after_stop_10ms"); ln_x("sum_before", s0); ln_x("sum_after", s1);
    ln_end((s0 == s1) ? V_PASS : V_FAIL);

    (void)start_check(3u);

    /* Rate change between two streams: the same triangle at 1 and at
     * 2 MSPS - the slope must come out twice as long in samples. */
    uint16_t slp = 0u;
    const uint32_t block = 2u * capture_half_len();
    tri_t r1, r2;
    uint32_t pos = 0u;
    bool ok = triangle_for(rate_hz(160u), &slp);
    wait_ticks(TICKS_PER_MS);
    ok = ok && grab_window(160u, false, &pos);
    if (ok) { tri_eval(capture_buffer() + pos, block - pos, &r1); }
    ok = ok && grab_window(80u, false, &pos);
    if (ok) { tri_eval(capture_buffer() + pos, block - pos, &r2); }
    ln_begin(4u); ln_u("ksps_a", ksps_of(160u)); ln_u("ksps_b", ksps_of(80u)); ln_u("slpdat", slp);
    if (!ok) { ln_s(" note=no_window"); ln_end(V_FAIL); }
    else {
        const float la = (r1.l_up + r1.l_dn) * 0.5f, lb = (r2.l_up + r2.l_dn) * 0.5f;
        ln_f("La_x100", la, 100u); ln_f("Lb_x100", lb, 100u);
        const float q = (la > 0.0f) ? (lb / la) : 0.0f;
        ln_f("ratio_x1000", q, 1000u);
        ln_end(((q > 1.98f) && (q < 2.02f)) ? V_PASS : V_FAIL);
    }
    (void)dac2_level_start(0x800u);
}

/* ------------------------------------------------------------------ *
 * S8 - the old open questions with the new instruments
 * ------------------------------------------------------------------ */
static void b2b_window(uint32_t p1, uint32_t p2, uint32_t bursts, uint32_t tag)
{
    const uint32_t block = 2u * capture_half_len();
    const uint32_t conv_hz = 1600000000u / (p1 * p2) / 8u;   /* 8 ADC clocks */
    uint16_t slp = 0u;
    (void)capture_set_pll(p1, p2);
    const bool dac_ok = triangle_for(conv_hz, &slp);
    wait_ticks(TICKS_PER_MS);
    const uint32_t rc = capture_oneshot_n(bursts);
    const uint32_t tk = capture_oneshot_ticks();
    (void)capture_settle();
    tri_t r;
    ln_begin(tag); ln_s(" back_to_back"); ln_u("pll", p1 * 10u + p2); ln_u("bursts", bursts);
    ln_u("set_ksps", conv_hz / 1000u); ln_u("rc", rc);
    ln_u("ksps_measured", timebase_ksps(bursts * block, tk)); ln_u("slpdat", slp);
    if (!dac_ok || (rc != 0u)) { ln_end(V_FAIL); return; }
    tri_eval(capture_buffer(), block, &r);
    ln_tri(&r);
    ln_u("model_x100", (dac2_slope_samples_x1000(conv_hz) + 5u) / 10u);
    ln_end(tri_grid_ok(&r) ? V_INFO : V_FAIL);
}

static void stage8(void)
{
    mark(8u);
    /* a) the CLKGEN6 divider, measured at the generator itself */
    static const uint16_t ratios[] = { 100u, 200u, 400u };
    for (uint32_t k = 0; k < 3u; k++) {
        const uint32_t rc = capture_set_clkdiv(ratios[k]);
        const uint32_t hz = clock_monitor_hz(CM_CLKGEN6);
        const uint32_t exp = (uint32_t)(320000000ull * 100u / ratios[k]);
        ln_begin(1u + k); ln_u("clk6div_x100", ratios[k]); ln_u("rc", rc);
        ln_u("clkgen6_hz", hz); ln_u("expect", exp);
        const uint32_t d = (hz > exp) ? (hz - exp) : (exp - hz);
        ln_end(((rc == CLKDIV_OK) && (hz != 0u) && (d <= exp / 100u)) ? V_PASS : V_FAIL);
    }
    (void)capture_set_clkdiv(100u);

    /* b) CLKGEN6 off: what the monitor sees, and whether a burst still
     * completes on the triangle - moving values or frozen ones. */
    uint16_t slp = 0u;
    (void)triangle_for(8000000u, &slp);
    (void)capture_settle();
    adc_deinit();
    clock_adc_off();
    const uint32_t hz_off = clock_monitor_hz(CM_CLKGEN6);
    (void)clock_adc_on();
    (void)adc_reinit();
    adc_set_mode_burst();
    /* The buffer zeroed first, so that what is in it afterwards was
     * written by this probe and not left over from an earlier window. */
    capture_fill(0u);
    const uint32_t rc = capture_clkoff_probe(4u);
    const volatile uint16_t *pb = capture_buffer();
    uint32_t written = 0u;
    for (uint32_t i = 0; i < 2u * capture_half_len(); i++) { if (pb[i] != 0u) { written++; } }
    tri_t r;
    tri_eval(pb, 2u * capture_half_len(), &r);
    ln_begin(4u); ln_s(" clkgen6_off"); ln_u("cm_hz", hz_off); ln_u("probe_rc", rc);
    ln_u("samples_written", written); ln_u("buffer_pp", r.pp);
    ln_end(V_INFO);

    /* c) back-to-back: 1 burst against the 100th of a stream. Repeated
     * samples in the stream would make its slopes longer in samples. */
    b2b_window(5u, 5u, 1u, 5u);
    b2b_window(5u, 5u, 100u, 6u);
    b2b_window(5u, 1u, 1u, 7u);
    b2b_window(5u, 1u, 100u, 8u);

    /* back to the chain's configuration */
    (void)capture_set_pll(5u, 1u);
    adc_set_mode_single(SCCP1_ADC_TRIGGER);
    (void)dac2_level_start(0x800u);
}

/* ------------------------------------------------------------------ *
 * S9 - the attempt
 * ------------------------------------------------------------------ */
static int32_t best_index(void)
{
    for (int32_t i = (int32_t)LADDER_LEN - 1; i >= 0; i--) {
        if ((!s4_run || s4_ok[i]) && (!s5_run || s5_ok[i]) && (!s6_run || s6_ok[i]) &&
            (s4_run || s5_run || s6_run)) {
            return i;
        }
    }
    return -1;
}

static int32_t g_best = -1;
static bool    g_s9_best_ok = false;

static void stage9(void)
{
    mark(9u);
    (void)dac2_level_start(0x800u);
    g_best = best_index();
    g_s9_best_ok = false;
    if (g_best >= 0) {
        ln_begin(1u); ln_u("chosen_ksps", ksps_of(ladder_n[g_best])); ln_s(" why=highest_passing_S4_S5_S6");
        ln_end(V_INFO);
        g_s9_best_ok = stream_for(ladder_n[g_best], s4_data[g_best], 15000u, 1000u, 2u, true);
    } else {
        ln_begin(1u); ln_s(" note=no_rate_passed_so_far"); ln_end(V_INFO);
    }
    if (g_best != (int32_t)IDX_8MSPS) {
        (void)stream_for(ladder_n[IDX_8MSPS], s4_data[IDX_8MSPS], 15000u, 1000u, 3u, g_best < 0);
    }
}

/* ------------------------------------------------------------------ *
 * The run
 * ------------------------------------------------------------------ */
static void summary(void)
{
    static const char *const name[10] = {
        "preconditions", "sccp1", "sccp_to_adc", "adc_to_dma", "counting",
        "grid", "stream", "start_stop", "old_questions", "attempt" };
    for (uint32_t s = 0; s < 10u; s++) {
        const uint16_t *t = tally[s];
        if ((t[0] | t[1] | t[2] | t[3]) == 0u) { continue; }
        ln_len = 0u;
        ln_s("@SUM S"); char c[12]; fmt_u(c, s); ln_s(c);
        ln_s(" "); ln_s(name[s]);
        ln_u("pass", t[V_PASS]); ln_u("fail", t[V_FAIL]); ln_u("skip", t[V_SKIP]); ln_u("info", t[V_INFO]);
        ln_s("\r\n"); ln[ln_len] = '\0';
        console_puts(ln);
    }
    ln_len = 0u;
    ln_s("@SUM rates");
    for (uint32_t i = 0; i < LADDER_LEN; i++) {
        char c[12]; fmt_u(c, ksps_of(ladder_n[i]));
        ln_s(" "); ln_s(c); ln_s(":");
        ln_s(s4_run ? (s4_ok[i] ? (s4_data[i] ? "D" : "C") : "c") : "-");
        ln_s(s5_run ? (s5_ok[i] ? "G" : "g") : "-");
        ln_s(s6_run ? (s6_ok[i] ? "S" : "s") : "-");
    }
    ln_s("\r\n"); ln[ln_len] = '\0';
    console_puts(ln);
    say("@SUM legend: C/c count ok/not (D = ok with IRQSEL=1, CH0DATA), G/g grid, S/s stream with CPU\r\n");
    if ((g_best >= 0) && g_s9_best_ok) {
        ln_len = 0u; ln_s("@SUM USE THIS RATE:");
        ln_u("ksps", ksps_of(ladder_n[g_best]));
        ln_s(s4_data[g_best] ? " src=CH0DATA irqsel=1" : " src=CH0RES irqsel=0");
        ln_s("\r\n"); ln[ln_len] = '\0';
        console_puts(ln);
    } else {
        say("@SUM NO RATE streams cleanly with the CPU processing - the example does not yet do what it says; see the first FAIL above\r\n");
    }
}

/* The simulator has no SCCP, ADC or DMA: the test says so and stops.
 * A run-time test rather than #ifdef, so that the simulator build still
 * compiles - and warns about - every line of it. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define CHAIN_ON_SIMULATOR  1
#else
#define CHAIN_ON_SIMULATOR  0
#endif

void chain_all(uint32_t first, uint32_t last)
{
    chain_stream_off();                       /* a running stream goes first */
    if (CHAIN_ON_SIMULATOR) {
        say("@S0.0 note=simulator_build_has_no_SCCP_ADC_DMA -> SKIP\r\n@END\r\n");
        return;
    }
    for (uint32_t s = 0; s < 10u; s++) { for (uint32_t v = 0; v < 4u; v++) { tally[s][v] = 0u; } }
    s4_run = s5_run = s6_run = false;
    for (uint32_t i = 0; i < LADDER_LEN; i++) {
        s4_ok[i] = s5_ok[i] = s6_ok[i] = false; s4_data[i] = false;
    }
    g_best = -1; g_s9_best_ok = false;
    g_gain = 1.0f; g_offs = 0.0f; g_tol = 150u;
    g_trig_hz = TRIG_HZ_NOMINAL;
    g_trigger_ok = true;

    say("\r\n@BEGIN chain test - SCCP1 -> ADC core 5 -> DMA0 -> ping-pong -> CPU, DAC2 on RA8\r\n");
    mark(0u);
    const bool ok = setup();
    if (last > 9u) { last = 9u; }
    for (uint32_t s = first; s <= last; s++) {
        if (!ok && (s > 0u)) {
            g_stage = s;
            ln_begin(0u); ln_s(" note=clock_tree_or_core_setup_failed"); ln_end(V_SKIP);
            continue;
        }
        if (!g_trigger_ok && (s >= 3u) && (s != 8u)) {
            g_stage = s;
            ln_begin(0u); ln_s(" note=S2_trigger_does_not_reach_the_ADC"); ln_end(V_SKIP);
            continue;
        }
        switch (s) {
        case 0u: stage0(); break;
        case 1u: stage1(); break;
        case 2u: stage2(); break;
        case 3u: stage3(); break;
        case 4u: stage4(); break;
        case 5u: stage5(); break;
        case 6u: stage6(); break;
        case 7u: stage7(); break;
        case 8u: stage8(); break;
        default: stage9(); break;
        }
    }
    g_stage = 10u;
    summary();
    restore();
    chain_mark = 0u;
    say("@END\r\n");
}

void chain_run(uint32_t ksps, uint32_t seconds)
{
    chain_stream_off();
    if (CHAIN_ON_SIMULATOR) {
        say("@S9.0 note=simulator_build -> SKIP\r\n");
        return;
    }
    for (uint32_t v = 0; v < 4u; v++) { tally[9][v] = 0u; }
    mark(9u);
    if (!setup()) {
        ln_begin(0u); ln_s(" note=clock_tree_or_core_setup_failed"); ln_end(V_FAIL);
        restore();
        chain_mark = 0u;
        return;
    }
    uint32_t n = (g_trig_hz / 1000u + ksps / 2u) / ksps;
    if (n < 4u) { n = 4u; }                   /* 40 MSPS is the ceiling      */
    if (seconds == 0u) { seconds = 10u; }
    if (seconds > 3600u) { seconds = 3600u; }
    const uint32_t snap_ms = (seconds <= SNAP_MAX) ? 1000u : (seconds * 1000u / SNAP_MAX);
    ln_begin(1u); ln_u("asked_ksps", ksps); ln_u("period_ticks", n); ln_u("ksps", ksps_of(n));
    ln_u("seconds", seconds); ln_end(V_INFO);
    (void)dac2_level_start(0x800u);
    (void)stream_for(n, false, seconds * 1000u, snap_ms, 2u, true);
    restore();
    chain_mark = 0u;
    say("@END\r\n");
}

/* ------------------------------------------------------------------ *
 * The chain as a standing stream ("stream on") - the example itself
 *
 * Set up as the test does, the DAC triangle on RA8 as the signal, the
 * triggered stream started with no end, and then the command RETURNS:
 * from here on main() serves every completed half through
 * capture_service(), exactly as the application will, and the console
 * stays free. Nothing prints by itself while it runs; "stream" asks.
 * ------------------------------------------------------------------ */
static bool     s_on     = false;
static uint32_t s_ticks  = 0u;
static uint16_t s_slpdat = 0u;

/* Baselines for chain_stream_grab_begin()'s per-cycle counters: the
 * lifetime counter as it stood after the previous grab (or after
 * "stream on", for the first one - all of them are 0 then, because
 * capture_chain_start() calls counters_clear()). */
static uint32_t g_grab_ov0, g_grab_la0, g_grab_mi0, g_grab_hv0;
static uint64_t g_grab_xf0;

static uint32_t period_for(uint32_t ksps)
{
    uint32_t n = (g_trig_hz / 1000u + ksps / 2u) / ksps;
    return (n < 4u) ? 4u : n;                 /* 40 MSPS is the ceiling      */
}

bool chain_stream_on(uint32_t ksps)
{
    chain_stream_off();
    if (CHAIN_ON_SIMULATOR || (ksps == 0u)) { return false; }
    g_trig_hz = TRIG_HZ_NOMINAL;
    if (!setup()) { restore(); return false; }
    const uint32_t n = period_for(ksps);
    uint16_t slp = 0u;
    (void)triangle_for(rate_hz(n), &slp);
    wait_ticks(TICKS_PER_MS);
    if (!capture_chain_start(n, SCCP_MODE_TIMER, 0u, false)) { restore(); return false; }
    s_on     = true;
    s_ticks  = n;
    s_slpdat = slp;
    g_grab_ov0 = 0u; g_grab_la0 = 0u; g_grab_mi0 = 0u; g_grab_hv0 = 0u; g_grab_xf0 = 0u;
    return true;
}

void chain_stream_off(void)
{
    if (!s_on) { return; }
    (void)capture_chain_stop();
    restore();
    s_on = false;
}

bool chain_streaming(void)
{
    return s_on;
}

bool chain_stream_grab_begin(chain_grab_t *g)
{
    if (!s_on) { return false; }
    if (!capture_chain_halt()) {
        /* The brake fired, or the chain was already down under us: leave
         * nothing half-configured, and let "stream" say it is off. */
        chain_stream_off();
        return false;
    }
    g->win     = capture_completed_half();
    g->win_len = capture_half_len();
    g->from    = (uint32_t)(g->win - capture_buffer());
    g->ksps    = ksps_of(s_ticks);
    const uint32_t ov = dma_overrun, la = late_service, mi = proc_missed, hv = blocks_done;
    const uint64_t xf = capture_transfers();
    g->overrun   = ov - g_grab_ov0;
    g->late      = la - g_grab_la0;
    g->missed    = mi - g_grab_mi0;
    g->halves    = hv - g_grab_hv0;
    g->transfers = (uint32_t)(xf - g_grab_xf0);
    g_grab_ov0 = ov; g_grab_la0 = la; g_grab_mi0 = mi; g_grab_hv0 = hv; g_grab_xf0 = xf;
    g->slpdat  = s_slpdat;
    g->dac_hz  = clock_dac_hz();
    return true;
}

bool chain_stream_grab_end(void)
{
    if (!s_on) { return false; }
    if (!capture_chain_resume()) {
        chain_stream_off();
        return false;
    }
    return true;
}

bool chain_stream_state(uint32_t *ksps, uint64_t *transfers, uint32_t *free_cyc)
{
    if (!s_on) { return false; }
    *ksps = ksps_of(s_ticks);
    *transfers = capture_transfers();
    /* The budget as in S6: a half lasts half_len * N / f_trig, 16 CPU
     * cycles per Timer1 tick, minus the mean processing time. */
    const uint32_t hl = capture_half_len();
    const uint32_t half_ticks = (uint32_t)(((uint64_t)hl * s_ticks * TIMEBASE_HZ) / g_trig_hz);
    const uint32_t pmean = (proc_count != 0u) ? (proc_ticks_sum / proc_count) : 0u;
    *free_cyc = (half_ticks > pmean) ? ((half_ticks - pmean) * CPU_PER_TICK / hl) : 0u;
    return capture_chain_active();            /* false once the brake fired  */
}

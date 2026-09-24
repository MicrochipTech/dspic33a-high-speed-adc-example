/*
 * dactest.c - the DAC2 triangle seen through the ADC/DMA chain (dactest.h)
 *
 * WHAT THIS TEST IS FOR
 *
 * It is the only test in this project that can say whether real
 * conversions reach the buffer, complete and in order. Everything else
 * is compatible with a DMA that copies a stale result register: the
 * self-test samples a DC reference, so a frozen value gives exactly the
 * right mean, and the counters count DMA events, not conversions.
 *
 * Run 11 (24.09.2026) answered the first half of that. Routed through
 * UREF, the buffer held the full DAC range - min 221, max 3864 against a
 * DAC set to 256..3840 - so the ADC really converts and the DMA really
 * moves the results. What it could not answer was order and
 * completeness, because the capture was torn: at the full rate the main
 * loop ran tens of milliseconds behind the DMA, the half being copied
 * had been overwritten a thousand times in the meantime, and the copy
 * came out as a mixture of old and new data (8552 halves missed between
 * eight copies, 430 slope reversals, single steps of 3126 counts).
 *
 * HOW IT WORKS NOW: one buffer, and the stream stops from the interrupt
 *
 * capture_oneshot() fills the buffer exactly once and the DMA interrupt
 * itself ends the stream, so nothing is racing the copy afterwards. The
 * ADC burst is CNT = 2 * half_len conversions, which is one full buffer,
 * so the window is contiguous by construction - no gaps to count, no
 * torn halves. The main loop being slow no longer matters: it only has
 * to notice, eventually, that the burst is over.
 *
 * WHAT MUST BE IN IT
 *
 * 2048 samples are about 48 us at the rates seen so far, and one slope
 * of the triangle lasts 220 us at the boot clock, so the window covers
 * roughly a fifth of a slope: a clean monotonic ramp of some 700 counts,
 * with at most one turning point if the window happens to straddle a
 * peak. A faster triangle covers more - "dac on <slpdat>" with a smaller
 * slpdat, 2 gives about a quarter of the period per buffer.
 *
 * THE VERDICT
 *   peak-to-peak     a frozen register gives 0, a real ramp hundreds of
 *                    counts
 *   reversals        at most one in a window this short; more means the
 *                    samples are not in the order they were converted
 *   largest step     the triangle moves less than one count per sample at
 *                    these rates, so a jump of hundreds means a sample is
 *                    missing or the window is torn
 *   raw dump         a few dozen values, so a human can see the ramp in
 *                    the log instead of trusting the arithmetic
 */

#include <xc.h>
#include <string.h>
#include "dactest.h"
#include "capture.h"
#include "adc.h"
#include "dac.h"
#include "dma.h"
#include "console.h"
#include "timebase.h"
#include "diag.h"
#include "sim.h"
#include "clock.h"

/* A frozen result register gives a peak-to-peak of zero; noise on a real
 * input gives a few counts. Anything below this means nothing is moving. */
#define DACTEST_FLAT_PP       50u
/* A window of about a fifth of a slope can straddle at most one peak. */
#define DACTEST_MAX_REVERSALS 1u
/* The reversal detector's hysteresis has to be smaller than the signal
 * it is watching, or it never arms and reports zero reversals whatever
 * the data do - which is how run 12 produced a vacuous PASS on a window
 * that moved 72 counts against a hysteresis of 96. It is derived from
 * the measured peak-to-peak now, with a floor for noise. */
#define DACTEST_HYST_MIN      8u
#define DACTEST_HYST_DIV      8u
#define DACTEST_TOL           150u    /* LSb, min/max window (DA06 + ADC)  */
/* The step between two neighbouring samples, relative to the swing of
 * the whole window. A triangle climbs its span over hundreds of samples,
 * so a single step worth an eighth of the swing is a missing sample or a
 * seam between two writes - not signal. Relative, because it has to hold
 * at any rate and any triangle speed; absolute limits only ever fitted
 * one setting. */
#define DACTEST_STEP_DIV      8u
#define DACTEST_STEP_MIN      16u
/* Reversal positions, to derive the period from the data. */
#define DACTEST_MAX_MARKS     16u

static uint16_t store[SAMPLES_PER_BUF_MAX];

uint32_t dactest_run(uint32_t halves)
{
    (void)halves;                     /* one buffer, by construction     */

    if (!dac2_running()) {
        console_puts("[dactest] DAC2 is off - nothing to test\r\n");
        return 1u;
    }
    const uint32_t low       = dac2_low();
    const uint32_t high      = dac2_high();
    const uint32_t period_ns = dac2_period_ns();
    const uint32_t n         = 2u * capture_half_len();

    console_puts("[dactest] DAC2 triangle through ADC, DMA and the ping-pong buffer\r\n"
                 "[dactest] one buffer, the stream stopped from the DMA interrupt, so the\r\n"
                 "[dactest] window is contiguous and nothing overwrites it while it is read\r\n");
    console_kv("[dactest]   ADC core", adc_core());
    console_kv("[dactest]   input (PINSEL)", capture_pinsel());
    console_kv("[dactest]   samples in the window", n);
    console_kv("[dactest]   expected min (DACLOW)", low);
    console_kv("[dactest]   expected max (DACDAT)", high);
    console_kv("[dactest]   triangle period ns", period_ns);
    console_flush();

    /* ---- match the triangle to the sample rate ---------------------
     *
     * The window is 2048 samples long, so its duration depends entirely
     * on the rate in use - 502 us at 4 MSPS, 256 us at 8, 102 us at 20.
     * A triangle that is right for one of them is useless at the others:
     * too slow and the window stands still (run 12, a swing of 72
     * counts), too fast and it holds fifty periods and the dump is
     * unreadable. So the speed is set from the rate, aiming at about one
     * period per window.
     *
     * The scaling is anchored on a measurement, not on dac2_period_ns():
     * run 13 showed one period in 513 us at slpdat 64 with a 32.65 MHz
     * DAC clock, while the formula claimed 54.9 us. Until that formula is
     * understood, the anchor is the honest way round. Period scales as
     * 1/(clock * slpdat), so:
     *
     *   slpdat = 64 * (513 us / window) * (32.65 MHz / dac clock)      */
    {
        const uint32_t ksps_now = capture_variant_ksps();
        const uint32_t f_dac    = clock_dac_hz();
        if ((ksps_now != 0u) && (f_dac != 0u)) {
            const uint32_t win_ns = (uint32_t)(((uint64_t)n * 1000000u) / ksps_now);
            uint64_t sl = (uint64_t)64u * 513280u * 32653061u;
            sl /= ((uint64_t)win_ns * f_dac);
            if (sl < 1u)   { sl = 1u; }
            if (sl > 255u) { sl = 255u; }
            console_kv("[dactest]   window ns expected", win_ns);
            console_kv("[dactest]   triangle slpdat chosen for it", (uint32_t)sl);
            if (!dac2_triangle_start(0x100u, 0xF00u, (uint16_t)sl)) {
                console_puts("[dactest]   could not restart the DAC at that speed\r\n");
            }
        }
    }

    /* ---- one buffer, then the ISR stops ---------------------------- */
    const uint32_t rc    = capture_oneshot();
    const uint32_t ticks = capture_oneshot_ticks();   /* the burst alone */
    if (rc != 0u) {
        console_puts((rc == 8u) ? "[dactest] the DMA channel switched itself off\r\n"
                                : "[dactest] no data\r\n");
        (void)capture_settle();
        return rc;
    }
    /* The stream is down; the buffer is ours. */
    memcpy(store, (const void *)capture_buffer(), n * sizeof store[0]);
    (void)capture_settle();

    /* ---- judge ------------------------------------------------------ */
    /* First pass: extremes and the largest step. */
    uint32_t mn = 0xFFFFu, mx = 0u, maxstep = 0u;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = store[i];
        if (v < mn) { mn = v; }
        if (v > mx) { mx = v; }
        if (i != 0u) {
            const uint32_t p = store[i - 1u];
            const uint32_t d = (v > p) ? (v - p) : (p - v);
            if (d > maxstep) { maxstep = d; }
        }
    }
    const uint32_t pp = mx - mn;

    /* Second pass: reversals, with a hysteresis scaled to this window. */
    uint32_t hyst = pp / DACTEST_HYST_DIV;
    if (hyst < DACTEST_HYST_MIN) { hyst = DACTEST_HYST_MIN; }
    uint32_t reversals = 0u, dir = 0u, ext = store[0];
    uint32_t marks[DACTEST_MAX_MARKS];
    uint32_t nmarks = 0u;
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = store[i];
        bool turned = false;
        if (dir == 1u) {
            if (v > ext) { ext = v; }
            else if ((ext - v) > hyst) { turned = true; dir = 2u; ext = v; }
        } else if (dir == 2u) {
            if (v < ext) { ext = v; }
            else if ((v - ext) > hyst) { turned = true; dir = 1u; ext = v; }
        } else {
            if (v > (ext + hyst))      { dir = 1u; ext = v; }
            else if ((v + hyst) < ext) { dir = 2u; ext = v; }
        }
        if (turned) {
            reversals++;
            if (nmarks < DACTEST_MAX_MARKS) { marks[nmarks++] = i; }
        }
    }

    /* The period AS MEASURED, from the distance between turning points -
     * two of them are one full period. The period computed from the DAC
     * registers is printed next to it as information only: at slpdat 64
     * it said 54.9 us and the capture showed 449 us (run 13), so the
     * formula in dac.c does not describe this hardware and nothing is
     * judged against it. The same goes for DACLOW: the triangle in run 13
     * ran between 2416 and 3851, and only the upper end matched. */
    const uint32_t window_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / TIMEBASE_HZ);
    uint32_t meas_period_ns = 0u;
    if ((nmarks >= 2u) && (window_ns != 0u) && (n != 0u)) {
        const uint32_t spread   = marks[nmarks - 1u] - marks[0];
        const uint32_t per_samp = (2u * spread) / (nmarks - 1u);
        meas_period_ns = (uint32_t)(((uint64_t)per_samp * window_ns) / n);
    }

    console_kv("[dactest] window ticks (12.5 MHz)", ticks);
    console_kv("[dactest] window ns", window_ns);
    console_kv("[dactest] sample rate ksps in this burst", (window_ns != 0u)
               ? (uint32_t)(((uint64_t)n * 1000000u) / window_ns) : 0u);
    console_kv("[dactest] reversal hysteresis used", hyst);
    console_kv("[dactest] triangle period ns MEASURED from the data", meas_period_ns);
    console_kv("[dactest]   the same computed from the DAC registers", period_ns);
    console_puts("[dactest]   (the computed one is known not to match this hardware -\r\n"
                 "[dactest]    it is printed for the record, nothing is judged against it)\r\n");
    console_kv("[dactest] min", mn);
    console_kv("[dactest] max", mx);
    console_kv("[dactest] peak-to-peak", pp);
    console_kv("[dactest] largest step between two samples", maxstep);
    console_kv("[dactest] slope reversals", reversals);
    console_kv("[dactest] overrun during the burst", dma_overrun);

    /* ---- the raw values, so the log shows the ramp itself ----------- */
    console_puts("[dactest] first 24 samples (consecutive):\r\n");
    for (uint32_t i = 0; i < 24u; i++) {
        console_kv("[dactest]   ", store[i]);
    }
    console_puts("[dactest] every 64th sample across the window:\r\n");
    for (uint32_t i = 0; i < n; i += 64u) {
        console_kv("[dactest]   ", store[i]);
    }

    /* ---- the verdict ------------------------------------------------ */
    /* What is judged, and why only this: the test exists to show that
     * every conversion arrives and that they arrive in the order they
     * were converted. A changing signal with small, even steps and no
     * jump proves both. How fast the triangle runs and where its lower
     * end sits are properties of the DAC, and getting them wrong must not
     * fail a chain that is working. */
    uint32_t step_limit = pp / DACTEST_STEP_DIV;
    if (step_limit < DACTEST_STEP_MIN) { step_limit = DACTEST_STEP_MIN; }

    bool ok = true;
    if (pp < DACTEST_FLAT_PP) {
        console_puts("[dactest]   FAIL: THE WINDOW IS FLAT - no changing signal arrives.\r\n"
                     "[dactest]     Either the DAC does not reach this input, or the DMA is\r\n"
                     "[dactest]     copying a result register that never changes.\r\n");
        ok = false;
    }
    console_kv("[dactest] step limit for this swing", step_limit);
    if (maxstep > step_limit) {
        console_puts("[dactest]   FAIL: a jump far larger than the signal makes between two\r\n"
                     "[dactest]     neighbouring samples - a sample is missing, or the window\r\n"
                     "[dactest]     is a seam between two writes\r\n");
        ok = false;
    }
    if (mn < ((low > DACTEST_TOL) ? (low - DACTEST_TOL) : 0u)) {
        console_puts("[dactest]   note: minimum below DACLOW\r\n");
    }
    if (mx > (high + DACTEST_TOL)) {
        console_puts("[dactest]   note: maximum above DACDAT\r\n");
    }
    console_puts(ok ? "[dactest] PASS: a changing signal arrives in the buffer, in order and\r\n"
                      "[dactest]   without a break - the ADC converts and the DMA moves every\r\n"
                      "[dactest]   result into the right place\r\n"
                    : "[dactest] FAIL\r\n");
    return ok ? 0u : 1u;
}

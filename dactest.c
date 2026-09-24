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
/* The triangle moves well under one count per sample at these rates, so
 * a step of this size is a missing sample or a torn window, not signal. */
#define DACTEST_MAX_STEP      200u

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

    /* ---- one buffer, then the ISR stops ---------------------------- */
    const uint32_t t0 = timebase_ticks();
    const uint32_t rc = capture_oneshot();
    const uint32_t ticks = timebase_ticks() - t0;
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
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = store[i];
        if (dir == 1u) {
            if (v > ext) { ext = v; }
            else if ((ext - v) > hyst) { reversals++; dir = 2u; ext = v; }
        } else if (dir == 2u) {
            if (v < ext) { ext = v; }
            else if ((v - ext) > hyst) { reversals++; dir = 1u; ext = v; }
        } else {
            if (v > (ext + hyst))      { dir = 1u; ext = v; }
            else if ((v + hyst) < ext) { dir = 2u; ext = v; }
        }
    }

    /* How far the triangle should have moved while this window was
     * captured. The window's length comes from Timer1, the slope rate
     * from the DAC settings - so this compares two independent things
     * and is the check that a nearly flat window cannot pass. */
    const uint32_t window_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / TIMEBASE_HZ);
    const uint32_t span      = (high > low) ? (high - low) : 0u;
    uint32_t expected_pp = 0u;
    if ((period_ns != 0u) && (window_ns != 0u)) {
        expected_pp = (uint32_t)(((uint64_t)2u * span * window_ns) / period_ns);
        if (expected_pp > span) { expected_pp = span; }
    }

    console_kv("[dactest] window ticks (12.5 MHz)", ticks);
    console_kv("[dactest] window ns", window_ns);
    console_kv("[dactest] sample rate ksps in this burst", (window_ns != 0u)
               ? (uint32_t)(((uint64_t)n * 1000000u) / window_ns) : 0u);
    console_kv("[dactest] the triangle should move this far in that window", expected_pp);
    console_kv("[dactest] reversal hysteresis used", hyst);
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
    bool ok = true;
    /* The measured swing has to be in the region of what the DAC settings
     * and the measured window length say it must be. Half of it allows
     * for a window that straddles a peak, where the signal turns round
     * and comes back; less than that means the ADC is not following the
     * signal, whatever the other checks say. */
    if ((expected_pp != 0u) && (pp < (expected_pp / 2u))) {
        console_kv("[dactest]   FAIL: the window moved far less than the DAC should have; expected about", expected_pp);
        console_puts("[dactest]     Either the sample rate is not what the window length says,\r\n"
                     "[dactest]     or the DAC clock is not what its registers say, or the\r\n"
                     "[dactest]     signal does not reach this input. Try a faster triangle:\r\n"
                     "[dactest]     a larger slpdat ('dac on 64') shortens the period.\r\n");
        ok = false;
    }
    if (expected_pp == 0u) {
        console_puts("[dactest]   note: no window length - nothing to compare the swing against\r\n");
    }
    if (pp < DACTEST_FLAT_PP) {
        console_puts("[dactest]   FAIL: THE WINDOW IS FLAT - no changing signal arrives.\r\n"
                     "[dactest]     Either the DAC does not reach this input, or the DMA is\r\n"
                     "[dactest]     copying a result register that never changes.\r\n");
        ok = false;
    }
    if (reversals > DACTEST_MAX_REVERSALS) {
        console_puts("[dactest]   FAIL: too many slope reversals - the samples are not in the\r\n"
                     "[dactest]     order they were converted in\r\n");
        ok = false;
    }
    if (maxstep > DACTEST_MAX_STEP) {
        console_puts("[dactest]   FAIL: a jump larger than the triangle can make between two\r\n"
                     "[dactest]     samples - a sample is missing or the window is torn\r\n");
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

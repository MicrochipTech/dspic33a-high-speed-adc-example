/*
 * dactest.c - the DAC2 triangle seen through the ADC/DMA chain (dactest.h)
 *
 * WHAT THIS TEST IS FOR, since 24.09.2026 (run 10)
 *
 * It is no longer a nice-to-have. It is the only test in this project
 * that can say whether real conversions reach the buffer at all.
 *
 * Everything else measured so far is compatible with a DMA that copies a
 * stale result register over and over: the self-test samples a DC
 * reference, so a frozen value gives exactly the right mean; the counters
 * count DMA events, not conversions; and the delivered "rate" followed
 * neither the CLKGEN6 divider nor the PLL1 output dividers, came out
 * ABOVE the datasheet's 40 MSPS, and did not stop when CLKGEN6 was
 * switched off (docs/HARDWARE-LOG.md runs 8 to 10). A changing, known
 * signal is the only thing that can tell the two apart.
 *
 * HOW IT WORKS, and why it is built this way
 *
 * Capture first, analyse afterwards. The halves are copied into RAM and
 * nothing else happens while the stream runs; the judgement comes after
 * capture_settle(). The earlier version analysed each half as it arrived,
 * which at the full rate took so long under the overrun interrupt storm
 * that half a million overruns piled up and the brake stopped the test
 * after the first half (run 10). A memcpy of 2 KB costs about a thousand
 * cycles and easily fits in the 24 us a half lasts, even with the storm
 * stealing most of the CPU.
 *
 * Eight halves are 8192 samples, about 200 us at the rates seen so far.
 * The DAC triangle has a 439 us period at the boot clock setting, so one
 * slope lasts 220 us: the capture covers nearly a full slope, and what
 * must appear in the buffer is a clean monotonic ramp of some 1600 counts.
 *
 * THE VERDICT
 *   peak-to-peak     a frozen register gives 0, a real ramp some thousand
 *                    counts. THIS is the question the test exists for.
 *   reversals        a slope has at most one turning point in 200 us; many
 *                    reversals mean the samples are not in order
 *   gaps             halves that were completed but not copied - then the
 *                    stored sequence is not contiguous and the ramp would
 *                    show steps that are not the DAC's
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

/* Halves kept in RAM. Eight of them are 16 KB of the 64 KB on the part -
 * the DMA buffer is 4 KB and the rest of the program a few hundred bytes,
 * so this fits with room to spare. More would buy a longer window and
 * cost RAM that a real application would want. */
#define DACTEST_STORE_HALVES  8u

/* A frozen result register gives a peak-to-peak of zero; noise on a real
 * input gives a few counts. Anything below this means nothing is moving. */
#define DACTEST_FLAT_PP       100u
/* One slope of the triangle can turn over at most once in the captured
 * window; a handful allows for noise at the turning point. */
#define DACTEST_MAX_REVERSALS 4u
#define DACTEST_HYST          96u     /* LSb, reversal detector hysteresis */
#define DACTEST_TOL           150u    /* LSb, min/max window (DA06 + ADC)  */

static uint16_t store[DACTEST_STORE_HALVES][SAMPLES_PER_HALF_MAX];

uint32_t dactest_run(uint32_t halves)
{
    if (!dac2_running()) {
        console_puts("[dactest] DAC2 is off - nothing to test\r\n");
        return 1u;
    }
    if (halves > DACTEST_STORE_HALVES) { halves = DACTEST_STORE_HALVES; }
    if (halves < 2u)                   { halves = 2u; }

    const uint32_t low       = dac2_low();
    const uint32_t high      = dac2_high();
    const uint32_t period_ns = dac2_period_ns();
    const uint32_t half      = capture_half_len();

    console_puts("[dactest] DAC2 triangle through ADC, DMA and the ping-pong buffer\r\n"
                 "[dactest] capture first, judge afterwards - nothing is computed while\r\n"
                 "[dactest] the stream runs, so the overrun storm cannot stop the test\r\n");
    console_kv("[dactest]   ADC core", adc_core());
    console_kv("[dactest]   input (PINSEL)", capture_pinsel());
    console_kv("[dactest]   halves captured", halves);
    console_kv("[dactest]   samples per half", half);
    console_kv("[dactest]   expected min (DACLOW)", low);
    console_kv("[dactest]   expected max (DACDAT)", high);
    console_kv("[dactest]   triangle period ns", period_ns);

    /* ---- capture: copy and nothing else ---------------------------- */
    console_puts("[dactest] settling\r\n");
    console_flush();
    (void)capture_settle();
    counters_clear();

    uint32_t got  = 0u;
    uint32_t gaps = 0u;
    uint32_t last = blocks_done;
    uint32_t n    = WAIT_LIMIT;

    console_puts("[dactest] capturing\r\n");
    console_flush();
    const uint32_t t0 = timebase_ticks();
    capture_start();
    while (got < halves) {
        SIM_DMA_TICK();
        if (blocks_done == last) {
            if (capture_overrun_aborted()) {
                (void)capture_settle();
                console_puts("[dactest] stopped by the overrun brake\r\n");
                return 9u;
            }
            if (!dma0_enabled()) {
                (void)capture_settle();
                console_puts("[dactest] the DMA channel switched itself off\r\n");
                return 8u;
            }
            if (--n == 0u) {
                (void)capture_settle();
                console_puts("[dactest] no data\r\n");
                return 6u;
            }
            continue;
        }
        /* More than one half completed since the last copy: the stored
         * sequence has a hole and is not contiguous in time. */
        if ((blocks_done - last) > 1u) { gaps += (blocks_done - last) - 1u; }
        last = blocks_done;
        n    = WAIT_LIMIT;
        memcpy(store[got], (const void *)capture_completed_half(), half * sizeof store[0][0]);
        got++;
    }
    const uint32_t ticks = timebase_ticks() - t0;
    (void)capture_settle();           /* stream down before anything else */

    /* ---- judge, with the stream stopped ---------------------------- */
    const uint32_t samples = got * half;
    const uint32_t ksps    = timebase_ksps(samples, ticks);

    uint32_t mn = 0xFFFFu, mx = 0u;
    uint32_t reversals = 0u, maxstep = 0u;
    uint32_t dir = 0u, ext = 0u, prev = 0u;
    bool     first = true;

    for (uint32_t h = 0; h < got; h++) {
        for (uint32_t i = 0; i < half; i++) {
            const uint32_t v = store[h][i];
            if (v < mn) { mn = v; }
            if (v > mx) { mx = v; }
            if (first) { first = false; ext = v; prev = v; continue; }
            const uint32_t d = (v > prev) ? (v - prev) : (prev - v);
            if (d > maxstep) { maxstep = d; }
            prev = v;
            if (dir == 1u) {
                if (v > ext) { ext = v; }
                else if ((ext - v) > DACTEST_HYST) { reversals++; dir = 2u; ext = v; }
            } else if (dir == 2u) {
                if (v < ext) { ext = v; }
                else if ((v - ext) > DACTEST_HYST) { reversals++; dir = 1u; ext = v; }
            } else {
                if (v > (ext + DACTEST_HYST))      { dir = 1u; ext = v; }
                else if ((v + DACTEST_HYST) < ext) { dir = 2u; ext = v; }
            }
        }
    }
    const uint32_t pp = mx - mn;

    console_kv("[dactest] samples captured", samples);
    console_kv("[dactest] ksps measured", ksps);
    console_kv("[dactest] halves missed between copies (gaps)", gaps);
    console_kv("[dactest] min", mn);
    console_kv("[dactest] max", mx);
    console_kv("[dactest] peak-to-peak", pp);
    console_kv("[dactest] largest step between two samples", maxstep);
    console_kv("[dactest] slope reversals", reversals);
    console_kv("[dactest] overrun during the capture", dma_overrun);

    /* ---- the raw values, so the log shows the ramp itself ----------- */
    console_puts("[dactest] first 16 samples (consecutive):\r\n");
    for (uint32_t i = 0; i < 16u; i++) {
        console_kv("[dactest]   ", store[0][i]);
    }
    console_puts("[dactest] every 512th sample across the capture:\r\n");
    for (uint32_t k = 0; k < samples; k += 512u) {
        console_kv("[dactest]   ", store[k / half][k % half]);
    }

    /* ---- the verdict ------------------------------------------------ */
    bool ok = true;
    if (pp < DACTEST_FLAT_PP) {
        console_puts("[dactest]   FAIL: THE BUFFER IS FLAT - no changing signal arrives.\r\n"
                     "[dactest]     Either the DAC does not reach this input, or the DMA is\r\n"
                     "[dactest]     copying a result register that never changes. In the\r\n"
                     "[dactest]     second case no rate measured so far means anything.\r\n");
        ok = false;
    }
    if (reversals > DACTEST_MAX_REVERSALS) {
        console_puts("[dactest]   FAIL: too many slope reversals - the samples are not in order\r\n");
        ok = false;
    }
    if (gaps != 0u) {
        console_puts("[dactest]   FAIL: halves were completed but not copied - the captured\r\n"
                     "[dactest]     sequence is not contiguous, so the shape cannot be judged\r\n");
        ok = false;
    }
    if (mn < (low > DACTEST_TOL ? low - DACTEST_TOL : 0u)) {
        console_puts("[dactest]   note: minimum below DACLOW - more than the DAC should emit\r\n");
    }
    if (mx > (high + DACTEST_TOL)) {
        console_puts("[dactest]   note: maximum above DACDAT - more than the DAC should emit\r\n");
    }
    console_puts(ok ? "[dactest] PASS: a changing signal arrives in the buffer, in order and\r\n"
                      "[dactest]   without gaps - the ADC really converts and the DMA really\r\n"
                      "[dactest]   moves the results\r\n"
                    : "[dactest] FAIL\r\n");
    return ok ? 0u : 1u;
}

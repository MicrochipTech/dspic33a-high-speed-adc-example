/*
 * dactest.c - the DAC2 triangle seen through the ADC/DMA chain (dactest.h)
 *
 * The signal is known exactly (dac.c), so the judgement can be hard:
 *   min, max   within DACTEST_TOL of DACLOW / DACDAT (DAC and ADC are both
 *              12-bit and VDD-referred; DAC gain error up to 35 LSb,
 *              DA06, plus ADC errors - 150 LSb covers it)
 *   frequency  slope reversals / 2 over the captured time, against the
 *              period from the DAC settings, 10 %
 *   jumps      a step between two consecutive samples larger than four
 *              expected steps plus 64: a lost sample (DMA overrun) or a
 *              glitch. The expected step is 2 * (DACDAT - DACLOW) * f /
 *              Fs, from the nominal rate (40 MSPS assumed for B2B).
 * Each half is copied out of the DMA buffer right after it completes
 * (2 KB, a few microseconds) so that the evaluation never races the DMA
 * filling it again: at 40 MSPS a half lives 25.6 us.
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

#define DACTEST_TOL       150u       /* LSb, min/max window               */
#define DACTEST_HYST      96u        /* LSb, reversal detector hysteresis */
#define DACTEST_FREQ_PCT  10u

static uint16_t copy[SAMPLES_PER_HALF_MAX];

uint32_t dactest_run(uint32_t halves)
{
    if (!dac2_running()) {
        console_puts("[dactest] DAC2 is off - nothing to test\r\n");
        return 1u;
    }
    const uint32_t low       = dac2_low();
    const uint32_t high      = dac2_high();
    const uint32_t period_ns = dac2_period_ns();
    const uint32_t f_exp_hz  = (period_ns != 0u) ? (uint32_t)(1000000000ull / period_ns) : 0u;
    const uint32_t half = capture_half_len();
    /* The nominal rate of the divider setting in force: the expected
     * step per sample follows from it. */
    uint32_t ksps_nom = capture_nominal_ksps(capture_clkdiv());
    if (ksps_nom == 0u) { ksps_nom = 40000u; }
    /* expected step per sample, LSb: 2 * span * f / Fs */
    uint32_t step_exp = (uint32_t)(((uint64_t)2u * (high - low) * f_exp_hz) / ((uint64_t)ksps_nom * 1000u));
    if (step_exp == 0u) { step_exp = 1u; }
    const uint32_t jump_limit = 4u * step_exp + 64u;

    console_puts("[dactest] DAC2 triangle (RA8) through the ADC/DMA chain\r\n");
    console_kv("[dactest]   ADC core", adc_core());
    console_kv("[dactest]   halves", halves);
    console_kv("[dactest]   samples per half", half);
    console_kv("[dactest]   expected min (DACLOW)", low);
    console_kv("[dactest]   expected max (DACDAT)", high);
    console_kv("[dactest]   expected period ns", period_ns);
    console_kv("[dactest]   expected frequency Hz", f_exp_hz);
    console_kv("[dactest]   expected step per sample LSb", step_exp);
    console_kv("[dactest]   jump limit LSb", jump_limit);

    /* The defined start: stream stopped, DMA at the buffer start, no
     * leftovers from the tests before (capture_settle).
     *
     * The steps are traced and flushed one by one because run 7
     * (24.09.2026) stopped silently between the block above and the
     * first result line - twice, with two different terminals, and with
     * the console dead afterwards. Every wait on this path is bounded,
     * so the stop is not an ordinary wait; the trace says which call
     * swallows the CPU. Flushed after each line, otherwise the last one
     * is still in the transmit FIFO when the CPU stops. */
    console_puts("[dactest] settling\r\n");
    console_flush();
    (void)capture_settle();
    console_puts("[dactest] settled\r\n");
    console_flush();
    counters_clear();

    uint32_t mn = 0xFFFFu, mx = 0u, reversals = 0u, jumps = 0u;
    uint32_t dir = 0u;                      /* 0 unknown, 1 up, 2 down   */
    uint32_t ext = 0u;                      /* extreme since the reversal */
    bool     first = true;
    uint32_t prev = 0u;

    const uint32_t t0   = timebase_ticks();
    uint32_t       last = blocks_done;
    uint32_t       got  = 0u;
    uint32_t       n    = WAIT_LIMIT;
    console_puts("[dactest] starting the stream\r\n");
    console_flush();
    capture_start();
    console_puts("[dactest] stream started, collecting\r\n");
    console_flush();
    while (got < halves) {
        SIM_DMA_TICK();
        if (blocks_done == last) {
            if (capture_overrun_aborted()) { (void)capture_settle(); console_puts("[dactest] stopped by the overrun brake - this rate floods the CPU with interrupts\r\n"); return 9u; }
            if (!dma0_enabled()) { (void)capture_settle(); console_puts("[dactest] DMA channel switched itself off\r\n"); return 8u; }
            if (--n == 0u)       { (void)capture_settle(); console_puts("[dactest] no data\r\n"); return 6u; }
            continue;
        }
        last = blocks_done;
        n    = WAIT_LIMIT;
        memcpy(copy, (const void *)capture_completed_half(), half * sizeof copy[0]);
        got++;
        if (got == 1u) {              /* data flows; the loop is running */
            console_puts("[dactest] first half copied\r\n");
            console_flush();
        }
        for (uint32_t i = 0; i < half; i++) {
            const uint32_t v = copy[i];
            if (v < mn) { mn = v; }
            if (v > mx) { mx = v; }
            if (first) { first = false; ext = v; prev = v; continue; }
            const uint32_t d = (v > prev) ? v - prev : prev - v;
            if (d > jump_limit) { jumps++; }
            prev = v;
            /* Reversal detector with hysteresis: a new extreme extends
             * the current slope, a move of HYST against it is a turn. */
            if (dir == 1u) {
                if (v > ext) { ext = v; }
                else if (ext - v > DACTEST_HYST) { reversals++; dir = 2u; ext = v; }
            } else if (dir == 2u) {
                if (v < ext) { ext = v; }
                else if (v - ext > DACTEST_HYST) { reversals++; dir = 1u; ext = v; }
            } else {
                if (v > ext + DACTEST_HYST)      { dir = 1u; ext = v; }
                else if (v + DACTEST_HYST < ext) { dir = 2u; ext = v; }
            }
        }
    }
    const uint32_t ticks = timebase_ticks() - t0;
    (void)capture_settle();                 /* test over: DMA down      */

    const uint32_t samples = halves * half;
    const uint32_t ksps    = timebase_ksps(samples, ticks);
    /* f = reversals / 2 / (samples / Fs) = reversals * Fs / (2 * samples) */
    const uint32_t f_meas  = (uint32_t)(((uint64_t)reversals * ksps * 1000u) / ((uint64_t)2u * samples));

    console_kv("[dactest] samples", samples);
    console_kv("[dactest] ksps measured", ksps);
    console_kv("[dactest] min", mn);
    console_kv("[dactest] max", mx);
    console_kv("[dactest] reversals", reversals);
    console_kv("[dactest] frequency Hz measured", f_meas);
    console_kv("[dactest] jumps (> jump limit)", jumps);
    console_kv("[dactest] overrun during the test", dma_overrun);

    bool ok = true;
    const uint32_t dmin = (mn > low)  ? mn - low  : low - mn;
    const uint32_t dmax = (mx > high) ? mx - high : high - mx;
    if (dmin > DACTEST_TOL) { console_puts("[dactest]   FAIL: minimum not at DACLOW\r\n"); ok = false; }
    if (dmax > DACTEST_TOL) { console_puts("[dactest]   FAIL: maximum not at DACDAT\r\n"); ok = false; }
    if (f_exp_hz != 0u) {
        const uint32_t df = (f_meas > f_exp_hz) ? f_meas - f_exp_hz : f_exp_hz - f_meas;
        if (df > f_exp_hz * DACTEST_FREQ_PCT / 100u) {
            console_puts("[dactest]   FAIL: frequency not the triangle's\r\n"); ok = false;
        }
    }
    if (jumps != 0u) { console_puts("[dactest]   FAIL: jumps in the data (lost samples or glitches)\r\n"); ok = false; }
    if (mx < mn + DACTEST_HYST) { console_puts("[dactest]   FAIL: no signal (flat)\r\n"); ok = false; }
    console_puts(ok ? "[dactest] PASS: the DAC triangle arrives intact through ADC, DMA and the ping-pong buffer\r\n"
                    : "[dactest] FAIL\r\n");
    return ok ? 0u : 1u;
}

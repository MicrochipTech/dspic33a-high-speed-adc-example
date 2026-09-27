/*
 * meter.c
 *
 * The back-to-back measurement instruments of the ADC/DMA example, moved
 * out of capture.c on 27.09.2026 (P9.3, docs/IMPLEMENTATION-PLAN.md):
 * capture_process_bench(), capture_selftest(), capture_clkoff_probe(),
 * capture_oneshot()/capture_oneshot_n(), capture_measure_rate(). Every
 * body below is unchanged from capture.c except for two substitutions
 * forced by the move, both like-for-like:
 *
 *   - capture_process_bench() read the private `buf`/`half_len` capture.c
 *     could see directly; here it calls the equivalent public accessors
 *     capture_buffer()/capture_half_len() instead - same address, same
 *     value, no new accessor needed.
 *   - capture_selftest() and capture_measure_rate() read `half_len`
 *     directly for the same reason; both now call capture_half_len().
 *     half_len only ever changes with the stream stopped (capture.h), so
 *     reading it through the accessor mid-function is exactly as safe as
 *     the direct read was.
 *
 * Everything else these functions need - process_buffer(), wait_for_
 * blocks(), oneshot_left, oneshot_ticks - stays defined in capture.c and
 * is reached through the narrow, non-public capture_priv.h; see that
 * header for why (guard_check() and the DMA buffer stay capture.c-private,
 * and oneshot_left is also touched by the DMA interrupt handler).
 */

/* <xc.h> is not used directly here (no register access - everything goes
 * through adc.c/clock.c/capture.c's own functions), but adc.h pulls in
 * board.h, whose BOARD_EV74H48A branch checks __dsPIC33AK512MPS512__ - a
 * real-hardware build gets that as a compiler builtin from -mcpu
 * regardless, but the host trace harness's fake device header only
 * defines it where <xc.h> is included, exactly like capture.c/
 * chaintest.c/dactest.c already do for the same reason. */
#include <xc.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "meter.h"
#include "capture.h"
#include "capture_priv.h"
#include "adc.h"
#include "clock.h"
#include "timebase.h"
#include "console.h"
#include "stats.h"

/* Self-test input and window. ADxAN6 is the internal 15/16 * VDD
 * reference on every core and package (Table 16-2, p1224), which the
 * datasheet itself samples for gain calibration (Example 16-3, p1328) -
 * with SAMC = 3, because an internal reference is not a 50 ohm source.
 * Expected mean: 15/16 * 4096 = 3840; the window allows +-5 %. */
#define SELFTEST_PINSEL   6u
#define SELFTEST_SAMC     3u      /* 6.5 TAD = 81 ns, as in Example 16-3 */
#define SELFTEST_HALVES   6u      /* halves to let the switch settle    */
#define SELFTEST_MIN      3648u   /* 3840 - 5 %                          */
#define SELFTEST_MAX      4032u   /* 3840 + 5 %                          */

/* capture.h: the processing of one half timed with nothing else running -
 * the DMA idle, no interrupt - against which the load measured in a
 * stream shows what the DMA's bus traffic costs the CPU. Timer1 ticks. */
uint32_t capture_process_bench(void)
{
    const uint32_t t0 = timebase_ticks();
    process_buffer(capture_buffer(), capture_half_len());
    return timebase_ticks() - t0;
}

/* half_mean() is lib/stats.c since P2.2 (27.09.2026); capture_selftest()
 * below is its only caller here. */

uint32_t capture_selftest(uint32_t *mean)
{
    const uint8_t  keep_pinsel = adc_pinsel();
    const uint8_t  keep_samc   = adc_samc();
    const bool     was_running = capture_settle();   /* defined start   */
    uint32_t       rc;

    (void)capture_set_input(SELFTEST_PINSEL, SELFTEST_SAMC);
    capture_start();

    /* The switch takes effect at the next DONE, then a full burst runs
     * on the new input: wait long enough that the half we judge is the
     * reference and nothing else. */
    rc = wait_for_blocks(blocks_done + SELFTEST_HALVES);
    if (rc == 0u) {
        /* The cast drops `volatile`: the completed half is the one the
         * DMA finished last and is not writing (it fills the other half
         * until the next DONE), so half_mean() may read it as ordinary
         * memory. Same reasoning as completed_half_stats() in cli.c. */
        const uint32_t m = half_mean((const uint16_t *)capture_completed_half(), capture_half_len());
        selftest_mean = m;
        if (mean != NULL) { *mean = m; }
        if ((m < SELFTEST_MIN) || (m > SELFTEST_MAX)) { rc = 7u; }
    }

    if (rc == 0u) {
        console_kv("[selftest] mean on internal 15/16 VDD (expect ~3840)", selftest_mean);
    } else if (rc == 6u) {
        console_puts("[selftest] no DMA blocks arrived\r\n");
    } else if (rc == 7u) {
        console_kv("[selftest] mean outside 3648..4032", selftest_mean);
    } else {
        console_puts("[selftest] DMA channel disabled\r\n");
    }

    (void)capture_set_input(keep_pinsel, keep_samc);
    if (rc == 0u) {
        rc = wait_for_blocks(blocks_done + SELFTEST_HALVES);   /* settle */
    }
    if (!was_running) {
        capture_stop();
    }
    return rc;
}

/* ------------------------------------------------------------------ *
 * Delivered rate
 *
 * Timer1 (timebase.c) is only the stopwatch; the ADC clock produces the
 * rate. Measuring it is one thing and one thing only now: run `halves`
 * halves at whatever the divider is set to and divide the sample count
 * by the elapsed time. The judgement - does this rate match the ratio,
 * and did anything get lost - belongs to the caller, which prints it.
 * ------------------------------------------------------------------ */
uint32_t capture_clkoff_probe(uint32_t halves)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    (void)halves;
    return 6u;
#else
    /* The control experiment for the question runs 8 and 9 raised: is the
     * ADC really clocked from CLKGEN6? Table 16-1 says it is, and yet the
     * generator's divider has no effect on the conversion rate. So take
     * the core down, switch the generator OFF, bring the core back and
     * try to convert.
     *
     * Returns 0 if halves still arrive - which would mean the ADC is not
     * running off CLKGEN6 at all and explains everything at a stroke - or
     * 6/8 if nothing arrives, which is the expected, boring answer. The
     * generator and the core are restored either way. */
    (void)capture_settle();
    adc_deinit();
    clock_adc_off();
    const bool ready_off = adc_reinit();   /* does the core even come up? */
    counters_clear();
    const uint32_t target = blocks_done + halves;
    capture_start();
    const uint32_t rc = wait_for_blocks(target);
    (void)capture_settle();

    adc_deinit();                          /* restore, in the boot order  */
    (void)clock_adc_on();
    (void)adc_reinit();
    console_kv("[clkoff]   ADC core reported ready with the generator off", ready_off ? 1u : 0u);
    return rc;
#endif
}

uint32_t capture_oneshot(void)
{
    return capture_oneshot_n(1u);
}

uint32_t capture_oneshot_n(uint32_t bursts)
{
    if (bursts == 0u) { bursts = 1u; }
    /* Fill the buffer exactly once and stop. The ADC burst is CNT =
     * 2 * half_len conversions, so one burst is one full buffer: HALF at
     * the middle, DONE at the end, and the ISR does not restart it. The
     * buffer then holds one contiguous window that nothing is writing
     * any more, which is the only way to look at the data at a rate
     * where the main loop cannot keep up (run 11). */
    (void)capture_settle();
    counters_clear();
    const uint32_t target = blocks_done + (2u * bursts);  /* HALF and DONE each */
    oneshot_left = bursts;
    capture_start();
    /* The clock starts HERE, not before capture_settle(): taking the DMA
     * channel down and setting it up again costs a fixed 11.3 us, and
     * with it inside the window every rate came out low - by 2.2 % at
     * 4 MSPS and 17.8 % at 40, purely because the same 11.3 us is a
     * different share of a shorter burst (run 14). Corrected, the
     * delivered rate matches the setting to better than 1 % everywhere. */
    const uint32_t t0 = timebase_ticks();
    const uint32_t rc = wait_for_blocks(target);
    oneshot_ticks = timebase_ticks() - t0;
    oneshot_left  = 0u;
    capture_stop();
    return rc;
}

uint32_t capture_measure_rate(uint32_t halves, uint32_t *ksps)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    (void)halves;
    if (ksps != NULL) { *ksps = 0u; }
    return 0u;                        /* no ADC clock to measure        */
#else
    (void)capture_settle();           /* defined start                  */
    counters_clear();
    const uint32_t target = blocks_done + halves;
    const uint32_t t0     = timebase_ticks();
    capture_start();
    const uint32_t rc     = wait_for_blocks(target);
    const uint32_t ticks  = timebase_ticks() - t0;
    (void)capture_settle();           /* test over: DMA down            */
    if (rc != 0u) { return rc; }
    if (ksps != NULL) { *ksps = timebase_ksps(halves * capture_half_len(), ticks); }
    return 0u;
#endif
}

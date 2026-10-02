/*
 * test_pingpong.c - host-side test for src/core/pingpong.c (see pingpong.h)
 *
 * Built by tools\hosttest.bat against the real src/core/pingpong.c, not a
 * reimplementation - including pingpong_on_half(), which lives entirely in
 * the header as `static inline` (P9.1: no cross-module call from the DMA
 * interrupt), so this file exercises the identical code capture.c's
 * dma0_event() compiles in.
 *
 * A small stand-in buffer: half_len = 4, two halves back to back, with
 * recognisable values so last_sample and the completed-half pointer can
 * be checked exactly. Plain (non-volatile) locals are passed where a
 * `volatile T *` is expected - always a legal implicit conversion for a
 * pointer, adding a qualifier - so the test needs no casts. blocks_done/
 * ready_half/last_sample are read back into plain locals after each call
 * (pingpong.h: nothing is cached across calls, every address is an
 * argument), exactly the way capture.c reads its own globals afterwards.
 *
 * pingpong.h explains why `late` and `overrun` are NOT pingpong_t fields,
 * and why pingpong_on_half() takes one `second_half` flag rather than a
 * combined pair (both a measured ISR-cost regression, tools/fncmp.py
 * --count). The "late" case - both flags true in the same producer event
 * - is exercised below the same way dma0_event() handles it: two
 * pingpong_on_half() calls back to back, HALF then DONE, with no
 * service() call between them.
 */
#include <stdint.h>
#include <stddef.h>

#include "pingpong.h"
#include "check.h"

#define HALF_LEN   4u
#define GUARD_N    4u
#define GUARD_BASE 0x1000u

int main(void)
{
    uint16_t data[2u * HALF_LEN] = { 10, 11, 12, 13, 20, 21, 22, 23 };
    uint32_t guard[GUARD_N];
    uint32_t blocks_done = 0u;
    uint32_t ready_half  = 0xFFFFFFFFu;   /* sentinel: on_half must set it */
    uint16_t last_sample = 0u;
    pingpong_t pp = { 0 };                /* capture.c's instance is `static`,
                                           * zero by C's own rule; spelled
                                           * out here since this one is a
                                           * local */

    for (uint32_t i = 0; i < GUARD_N; i++) { guard[i] = GUARD_BASE + i; }

    /* ---- a fresh instance has serviced nothing ---- */
    CHECK_EQ(pp.missed, 0u);

    /* ---- plain alternation: HALF, service, DONE, service ---- */
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, false);
    CHECK_EQ(blocks_done, 1u);
    CHECK_EQ(ready_half, 0u);
    CHECK_EQ(last_sample, 13u);                    /* data[HALF_LEN-1]   */
    CHECK(pingpong_completed_half(data, HALF_LEN, ready_half) == &data[0]);
    CHECK(pingpong_service(&pp, blocks_done));      /* new half seen     */
    CHECK_EQ(pp.missed, 0u);
    CHECK(!pingpong_service(&pp, blocks_done));     /* nothing new since */

    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, true);
    CHECK_EQ(blocks_done, 2u);
    CHECK_EQ(ready_half, 1u);
    CHECK_EQ(last_sample, 23u);                     /* data[2*HALF_LEN-1] */
    CHECK(pingpong_completed_half(data, HALF_LEN, ready_half) == &data[HALF_LEN]);
    CHECK(pingpong_service(&pp, blocks_done));
    CHECK_EQ(pp.missed, 0u);

    /* ---- gap: two events with a service call in between, no loss ---- */
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, false);
    CHECK(pingpong_service(&pp, blocks_done));      /* blocks_done = 3   */
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, true);
    CHECK(pingpong_service(&pp, blocks_done));      /* blocks_done = 4   */
    CHECK_EQ(pp.missed, 0u);

    /* ---- late: both events with NO service() call in between, exactly
     * as dma0_event() calls this when both flags are set in one status
     * word - HALF applied, then DONE, blocks_done advances by two,
     * ready_half/last_sample end up as DONE left them (applied last) ---- */
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, false);
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, true);
    CHECK_EQ(blocks_done, 6u);
    CHECK_EQ(ready_half, 1u);                        /* DONE applied last */
    CHECK_EQ(last_sample, 23u);

    /* Fresh baseline before the next scenario: the late event above left
     * the service bookmark two blocks behind, which pingpong_service()
     * would otherwise book as "missed" too, conflating the two scenarios.
     * pingpong_counters_clear() re-syncs it without counting anything. */
    pingpong_counters_clear(&pp, blocks_done);
    CHECK_EQ(pp.missed, 0u);

    /* ---- skipped: two events with NO service() call in between, then
     * one service() call - missed grows by (delta - 1) = 1. Same shape as
     * "late" above, but a service() call now follows instead of another
     * pair of events - the point being pingpong_service(), not
     * pingpong_on_half(), notices the skip. ---- */
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, false);
    pingpong_on_half(data, HALF_LEN, &blocks_done, &ready_half, &last_sample, true);
    CHECK(pingpong_service(&pp, blocks_done));
    CHECK_EQ(pp.missed, 1u);

    /* ---- guard words: intact, then deliberately broken, then restored ---- */
    CHECK(pingpong_guard_ok(guard, GUARD_N, GUARD_BASE));
    const uint32_t saved = guard[2];
    guard[2] = 0xBADu;
    CHECK(!pingpong_guard_ok(guard, GUARD_N, GUARD_BASE));
    guard[2] = saved;                                /* revert            */
    CHECK(pingpong_guard_ok(guard, GUARD_N, GUARD_BASE));

    /* ---- counters_clear: missed re-armed, blocks_done kept (free-
     * running), seen_blocks caught up so the next service() sees nothing
     * new ---- */
    pingpong_counters_clear(&pp, blocks_done);
    CHECK_EQ(pp.missed, 0u);
    CHECK(blocks_done != 0u);                        /* free-running, kept */
    CHECK(!pingpong_service(&pp, blocks_done));       /* caught up          */

    return check_summary();
}

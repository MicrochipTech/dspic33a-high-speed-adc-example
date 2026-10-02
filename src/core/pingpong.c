/*
 * pingpong.c - the two-half buffer's bookkeeping (see pingpong.h)
 *
 * pingpong_on_half(), the ISR-rate function, is `static inline` in the
 * header (see the design note there); everything below is called at most
 * once per completed half, from the main loop, so it stays an ordinary
 * function.
 */

#include "pingpong.h"

const volatile uint16_t *pingpong_completed_half(volatile uint16_t *data, uint32_t half_len,
                                                 uint32_t ready_half)
{
    return &data[ready_half ? half_len : 0u];
}

bool pingpong_guard_ok(const volatile uint32_t *guard, uint32_t guard_words,
                       uint32_t guard_base)
{
    for (uint32_t i = 0; i < guard_words; i++) {
        if (guard[i] != guard_base + i) { return false; }
    }
    return true;
}

bool pingpong_service(pingpong_t *pp, uint32_t blocks_done)
{
    if (blocks_done == pp->seen_blocks) {
        return false;
    }
    if ((blocks_done - pp->seen_blocks) > 1u) {
        pp->missed += (blocks_done - pp->seen_blocks) - 1u;
    }
    pp->seen_blocks = blocks_done;
    return true;
}

void pingpong_counters_clear(pingpong_t *pp, uint32_t blocks_done)
{
    pp->missed      = 0u;
    pp->seen_blocks = blocks_done;
}

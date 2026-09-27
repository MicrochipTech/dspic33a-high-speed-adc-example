/*
 * pingpong.h - the two-half buffer's bookkeeping (pingpong.c)
 *
 * Moved out of capture.c on 27.09.2026 (P9.1, docs/REFACTORING-PROPOSAL.md
 * V7): what dma0_event() did for the buffer itself - which half just
 * completed, the last sample, the free-running block count - and what
 * capture_service() did to notice a half the main loop never got to and
 * to check the guard words behind the buffer. None of it is specific to
 * the ADC or the DMA, or even to an interrupt: the producer decides when
 * a half is "done" and reports it through pingpong_on_half(); a host
 * test drives the identical code without any of that existing.
 *
 * No driver include: this file and pingpong.c know neither DMA0STAT nor
 * <xc.h>. The caller (capture.c) decodes its own status bits itself and
 * calls pingpong_on_half() once per half that completed.
 *
 * Design note - why pingpong_t holds only `missed`, not `late`/`overrun`
 * too, although docs/IMPLEMENTATION-PLAN.md's P9.1 lists all three as
 * struct fields: a first version gave pingpong_on_half() a `pingpong_t *`
 * and an `overrun` flag and had it tally pp->late/pp->overrun, syncing
 * late_service from pp->late after every call. Measured with
 * tools/fncmp.py --count against the tree before this task (e52c234):
 * dma0_event() 124 -> 156 instructions (+32, +26 %), from three more
 * registers needing to stay live across the calls in between
 * (dma0_clear(), sccp1_stop()...) and an unconditional 3-instruction sync
 * of late_service on every single call, not only the rare late one.
 * `_DMA0Interrupt` itself (dma.c, unchanged) is unaffected either way: 42
 * instructions, 0 indirect calls.
 *
 * late_service and dma_overrun are DMA-channel facts capture.c already
 * decodes from `st` for its own reasons (dma_overrun has an early-exit
 * brake with nothing to do with the buffer, and the `late` condition -
 * both flags pending in one call - is one line capture.c can test for
 * itself with the exact same half_done/done_done booleans it already
 * has to compute to call pingpong_on_half()). Neither needs pingpong to
 * own it, and capture.c keeps incrementing both exactly as it always
 * did: zero change, zero cost, in the one function this task measures.
 * `missed` stays in pingpong_t because pingpong_service() runs from the
 * main loop, not the interrupt - no ISR budget applies to it.
 *
 * data/half_len and blocks_done/ready_half/last_sample are plain
 * arguments, not fields of pingpong_t or anything else this module
 * caches: at the call site inside dma0_event() they are the address of a
 * static array or a static variable in the SAME file, which -O1 folds to
 * a direct, fixed-address access after inlining - exactly what the
 * manual code being replaced always compiled to. Caching them in a
 * struct (set once, by an earlier call to a DIFFERENT function) would
 * make their value only known at RUNTIME to this function's compile, so
 * every access would cost a load of the cached pointer *and then* the
 * load or store through it - the mechanism behind the +32 instructions
 * above.
 */
#ifndef PINGPONG_H
#define PINGPONG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t seen_blocks;    /* pingpong_service()'s bookmark             */
    uint32_t missed;         /* halves skipped between two service() calls*/
} pingpong_t;

/* One completed half, called once from inside the caller's own
 * `if (<HALF event>)` and once from inside its `if (<DONE event>)` -
 * mirroring capture.c's former dma0_event() structure exactly, rather
 * than combining both into one call with two flags: an earlier version
 * did that (a single pingpong_on_half(half_done, done_done) called
 * unconditionally) and measured MORE instructions again, because the
 * compiler no longer had two independent, textually separate `if`
 * bodies to place code into - it had one shared call site used from
 * three different tests of the same bits (the caller's dispatch, this
 * function's own body, and the DONE-only code after it), which at -O1
 * triggered tail duplication instead of the reuse it looks like it
 * should. Splitting it back into "one call per branch" removed that.
 * second_half selects which half just completed (false = first, true =
 * second); the caller decides "both in one event" (late) itself, from
 * the same two conditions it already tests to call this - see
 * dma0_event() and the design note above for why that line stays there
 * instead of living here. data/half_len describe the buffer, and
 * blocks_done/ready_half/last_sample are the caller's own storage -
 * updated here so nothing above this module moves; see the design note
 * above for why they are arguments and not cached anywhere.
 *
 * `static inline`, defined here rather than in pingpong.c: dma0_event()
 * runs in the DMA interrupt, over a million times a second during an
 * overrun storm, and IMPLEMENTATION-PLAN.md P9.1 asks that the interrupt
 * not grow materially for this. Inlined into capture.c, with every
 * address below resolving to a compile-time constant at the call site,
 * the generated code is the same handful of loads/stores/increments the
 * inline version it replaces was - the identical body a host test calls
 * directly, with none of this mattering there. */
static inline void pingpong_on_half(volatile uint16_t *data, uint32_t half_len,
                                    volatile uint32_t *blocks_done,
                                    volatile uint32_t *ready_half,
                                    volatile uint16_t *last_sample,
                                    bool second_half)
{
    *ready_half  = second_half ? 1u : 0u;
    *last_sample = data[(second_half ? 2u * half_len : half_len) - 1u];
    (*blocks_done)++;
}

/* The half that completed last (capture.c's capture_completed_half()):
 * data/half_len describe the buffer, ready_half (0 or 1) which half. */
const volatile uint16_t *pingpong_completed_half(volatile uint16_t *data, uint32_t half_len,
                                                 uint32_t ready_half);

/* True if every one of the guard_words contiguous words at `guard` still
 * holds guard_base + i - the pattern capture_init() wrote there. */
bool pingpong_guard_ok(const volatile uint32_t *guard, uint32_t guard_words,
                       uint32_t guard_base);

/* Main-loop side: true if a half completed since the last call
 * (blocks_done, read by the caller once and passed in). If more than one
 * did, pp->missed grows by (delta - 1) - the halves the caller never got
 * to process. */
bool pingpong_service(pingpong_t *pp, uint32_t blocks_done);

/* Re-arm missed and the service bookmark (capture.c's counters_clear())
 * to the given blocks_done - not reset to 0: blocks_done itself is
 * free-running, exactly as before this task. */
void pingpong_counters_clear(pingpong_t *pp, uint32_t blocks_done);

#endif /* PINGPONG_H */

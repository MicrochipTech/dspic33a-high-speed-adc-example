/*
 * capture_priv.h - private state and helpers shared with meter.c only
 *
 * NOT part of capture.h's public, console-facing API - nothing outside
 * capture.c and meter.c includes this. It exists because P9.3 (27.09.2026)
 * moved capture_selftest()/capture_clkoff_probe()/capture_oneshot_n()/
 * capture_measure_rate() into src/meter/meter.c, but two things they need
 * stay in capture.c on purpose:
 *
 *   process_buffer()   also called by capture_service() (the main loop),
 *                       which stays in capture.c; and
 *   wait_for_blocks()   calls guard_check(), a static that reads the
 *                       guard words directly behind the private DMA
 *                       buffer (dma_buffer, capture.c) and is also called
 *                       by capture_service() - moving wait_for_blocks
 *                       would mean exposing the buffer's guard words
 *                       outside capture.c, or duplicating guard_check().
 *
 * Both were therefore left in place and made non-static, exactly the
 * pattern dma0_event()/adc_ch0_event() already use ("a plain extern",
 * CLAUDE.md). oneshot_left/oneshot_ticks are the same story the other
 * way round: capture_oneshot_n() (now in meter.c) sets them, but
 * dma0_event() - the DMA interrupt handler, capture.c - reads and
 * decrements oneshot_left on every DONE to know whether to restart the
 * burst. Keeping them as capture.c statics, merely no longer file-scope
 * private, means dma0_event() and _DMA0Interrupt are not touched at all
 * by this move.
 */
#ifndef CAPTURE_PRIV_H
#define CAPTURE_PRIV_H

#include <stdint.h>

/* One completed half, accumulated (capture.c). Unchanged body; still the
 * only place that touches the buffer's raw samples besides guard_check(). */
void     process_buffer(const volatile uint16_t *b, uint32_t n);

/* Block until blocks_done reaches target, or time out / DMA-disabled
 * (capture.c). Returns 0, 6 or 8 - see capture.c's comment. */
uint32_t wait_for_blocks(uint32_t target);

/* Bursts left to run before capture_oneshot_n()'s stream stops itself;
 * decremented by dma0_event() on every DONE (capture.c). */
extern volatile uint32_t oneshot_left;
/* Timer1 ticks of the last one-shot burst alone (capture.c); read back by
 * the public capture_oneshot_ticks(), which stays in capture.c too. */
extern volatile uint32_t oneshot_ticks;

#endif /* CAPTURE_PRIV_H */

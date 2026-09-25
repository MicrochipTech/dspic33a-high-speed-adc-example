/*
 * chaintest.h - the chain test: SCCP1 -> ADC -> DMA -> ping-pong -> CPU
 * (chaintest.c, plan in docs/CHAIN-TEST-PLAN.md)
 */
#ifndef CHAINTEST_H
#define CHAINTEST_H

#include <stdint.h>
#include <stdbool.h>

/* Stages first..last (0..9) of the chain test, then the summary and
 * @END. chain_all(0, 9) is "chain all", chain_all(n, 9) "chain from n",
 * chain_all(n, n) one stage. The clock tree and core 5 are always set up
 * first, quietly if stage 0 is not part of the run. Blocking, about a
 * minute for the whole run. Restores the boot configuration at the end. */
void chain_all(uint32_t first, uint32_t last);

/* The chain as the example would run it: set up, `seconds` of triggered
 * stream at about `ksps` (the nearest 160 MHz / N), the CPU processing
 * every half, one status line per second (printed afterwards, so that
 * printing does not disturb the stream), a verdict. */
void chain_run(uint32_t ksps, uint32_t seconds);

/* The chain as a standing stream, the way the example runs it:
 * chain_stream_on() sets it up at about `ksps` (the nearest 160 MHz / N,
 * 1..40000) with the DAC triangle on RA8 as the signal and returns; main()
 * then processes every half. chain_stream_off() stops it and restores the
 * boot configuration; chain_all(), chain_run() and the old tests call it
 * first. chain_stream_state(): rate, transfers since the start, free CPU
 * cycles per sample; false if no stream is on, or if it stopped itself
 * (overrun brake) - chain_streaming() still says one was started. */
bool chain_stream_on(uint32_t ksps);
void chain_stream_off(void);
bool chain_streaming(void);
bool chain_stream_state(uint32_t *ksps, uint64_t *transfers, uint32_t *free_cyc);

#endif /* CHAINTEST_H */

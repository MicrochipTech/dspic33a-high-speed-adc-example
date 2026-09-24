/*
 * chaintest.h - the chain test: SCCP1 -> ADC -> DMA -> ping-pong -> CPU
 * (chaintest.c, plan in docs/CHAIN-TEST-PLAN.md)
 */
#ifndef CHAINTEST_H
#define CHAINTEST_H

#include <stdint.h>

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

#endif /* CHAINTEST_H */

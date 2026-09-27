/*
 * dactest.h - does the DAC2 triangle come through the ADC/DMA chain? (dactest.c)
 */
#ifndef DACTEST_H
#define DACTEST_H

#include <stdint.h>

/* Capture `halves` buffer halves at the rate set now, copy
 * each completed half out of the ping-pong buffer as soon as it is
 * complete, and judge the data against the DAC settings: minimum and
 * maximum near DACLOW and DACDAT, the number of slope reversals against
 * the triangle period at the measured sample rate, and no jumps larger
 * than a few expected steps (a lost sample shows as a double step).
 * Prints [dactest] lines and PASS/FAIL. Returns 0 for PASS, 1 for FAIL,
 * 6/8 if no data came. Blocking, bounded. */
/* `bursts` bursts run back to back before the DMA interrupt stops the
 * stream; the buffer then holds the last of them. One burst looks at an
 * isolated capture, a hundred at a burst out of a running stream - and
 * comparing the two answers whether the converter really speeds up under
 * streaming or whether each conversion simply lands in the buffer more
 * than once. The triangle's period cannot change, so period in samples
 * divided by sample rate must agree at both counts. */
uint32_t dactest_run(uint32_t bursts);

#endif /* DACTEST_H */

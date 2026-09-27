/*
 * stats.h - min/max/mean of a block of samples (stats.c)
 *
 * Moved out of cli.c (half_stats) and capture.c (half_mean) on 27.09.2026
 * (P2.2), unchanged in what they compute, so that the host test
 * tests/host/test_stats.c reaches them.
 *
 * The interface is `const uint16_t *` WITHOUT volatile on purpose: the
 * library knows nothing about the DMA. The callers hand it the completed
 * half of the ping-pong buffer - the half the DMA has finished and is not
 * writing (capture_completed_half()) - and cast the volatile away at the
 * call site, where the comment says why that is safe.
 *
 * The mean is the truncating integer division acc / n (no rounding: a
 * true mean of 1.75 reports 1). acc is 32 bits wide, so the sum must fit:
 * with 12-bit samples that is any n below 2^20, with 16-bit values n up to
 * 65537. n must be at least 1 - n = 0 divides by zero, exactly as the
 * originals did.
 */
#ifndef STATS_H
#define STATS_H

#include <stdint.h>

/* min, max and mean of b[0..n). */
void half_stats(const uint16_t *b, uint32_t n,
                uint32_t *mn, uint32_t *mx, uint32_t *mean);

/* mean of b[0..n). */
uint32_t half_mean(const uint16_t *b, uint32_t n);

#endif /* STATS_H */

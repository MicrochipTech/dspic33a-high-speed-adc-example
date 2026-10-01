/*
 * sigproc.h - the signal-processing callback: one call per completed
 * half of the ping-pong buffer, ping and pong alike.
 *
 * Who calls it, and when: capture_service() (capture.c), from the main
 * loop - never from an interrupt - each time the DMA has filled one half,
 * and only while the processing is switched on: console command
 * "sigproc on|off" (the GUI's "signal processing" switch), off after reset.
 * The DMA is then writing the OTHER half, so the block handed over stays
 * still for one half period: n / sample rate, e.g. 2048 samples at
 * 8 MSPS = 256 us = 51 200 CPU cycles at 200 MHz, 25 cycles per sample.
 *
 * In place: the result goes back into x[0..n-1], the same half. "stream
 * grab" then sends exactly that half to the GUI, so the GUI shows the
 * processed data with no second buffer (its GRAB header says proc=1). The
 * grab makes sure of it: after halting the trigger it processes the last
 * completed half if the main loop has not yet, and a console command can
 * never interrupt this function half-way (cli.c's uart_rx_hook() holds
 * received bytes back while it runs and hands them to the parser right
 * after) - so the GUI never sees a half-processed block.
 *
 * The rules that follow:
 *   - return within one half period. Longer, and the main loop misses the
 *     next half ("missed", proc_missed, info->missed) while the DMA
 *     overwrites this one under the next half's processing. The time this
 *     function takes is measured around the call (proc_ticks_max/_sum/
 *     _count): "status" and the chain test's S6 load figures report it.
 *   - no console output, no waiting: one console line costs milliseconds,
 *     and a console command waits for this function to return.
 *   - the samples are 12-bit results, right-aligned in 16 bits; the
 *     pointer is 4-byte aligned and n is even, so two samples per 32-bit
 *     access is allowed (capture.c's process_buffer() reads that way). The
 *     GUI reads the values back as 12-bit numbers (bits 11:0), so a result
 *     meant for it stays in 0..4095.
 *   - "chain all", "test ..." and dactest judge the samples themselves:
 *     switch the processing off before running them.
 *
 * src/app/sigproc.c holds the body to fill in.
 */
#ifndef SIGPROC_H
#define SIGPROC_H

#include <stdint.h>

typedef struct {
    uint32_t half;      /* 0 = ping (first half of the buffer), 1 = pong   */
    uint32_t seq;       /* running number of completed halves (blocks_done)
                         * - consecutive calls differ by 1 unless halves
                         * were missed                                     */
    uint32_t missed;    /* halves the main loop never got to, since the
                         * counters were last cleared (proc_missed)        */
} sigproc_info_t;

/* x: the completed half, n samples (capture_half_len()); read it, and
 * write the result back into it. */
void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info);

#endif

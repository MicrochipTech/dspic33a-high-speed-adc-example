/*
 * sigproc.h - the signal-processing callback: one call per completed
 * half of the ping-pong buffer, ping and pong alike.
 *
 * Who calls it, and when: capture_service() (capture.c), from the main
 * loop - never from an interrupt - each time the DMA has filled one half,
 * and only while the processing is switched on: console command
 * "sigproc on|off" (the GUI's "signal processing" switch), off after reset.
 * The DMA is then writing the OTHER half, so the block handed over stays
 * still for one half period: n / sample rate, e.g. 1024 samples at
 * 8 MSPS = 128 us = 25 600 CPU cycles at 200 MHz, 25 cycles per sample.
 *
 * In place: the result goes back into x[0..n-1], the same half. "stream
 * grab" sends a whole ping-pong pair - ping then pong - and the stream
 * does not stop for it (since 01.10.2026): it moves on to the other pair,
 * and the pair just completed stands still while it is sent, so the GUI
 * shows the processed data with no second buffer (its GRAB header says
 * proc=1), and the processing never sees a gap in its input. Both halves
 * of that pair went through this function on the way (capture_service()
 * keeps running while the grab waits for the move), and a console command
 * can never interrupt this function half-way (cli.c's uart_rx_hook()
 * holds received bytes back while it runs and hands them to the parser
 * right after) - so the GUI never sees a half-processed block.
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
 * src/core/sigproc.c holds the body to fill in.
 */
#ifndef SIGPROC_H
#define SIGPROC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t half;      /* 0 = ping (first half of the buffer), 1 = pong   */
    uint32_t seq;       /* running number of completed halves (blocks_done)
                         * - consecutive calls differ by 1 unless halves
                         * were missed                                     */
    uint32_t missed;    /* halves the main loop never got to, since the
                         * counters were last cleared (proc_missed)        */
    uint32_t gap;       /* 1: this block does NOT follow the last one this
                         * function saw - the first after "sigproc on", the
                         * first of a (re)started stream, or halves missed
                         * in between. A filter re-starts its state here
                         * instead of carrying it across the gap.        */
} sigproc_info_t;

/* x: the completed half, n samples (capture_half_len()); read it, and
 * write the result back into it. */
void sigproc_block(uint16_t *x, uint32_t n, const sigproc_info_t *info);

/* What sigproc_block() does (02.10.2026, sigproc.c; console "sigproc ...",
 * the GUI's signal processing card): one filter at fs/8 on the block, in
 * place, and independently a Goertzel detector for a tone at fs/16 in the
 * block as it came in. Both off after reset. The console runs a command
 * never while sigproc_block() runs (cli.c's uart_rx_hook()), so a change
 * lands between two blocks. */
typedef enum {
    SIGPROC_OFF = 0,    /* the block passes unchanged                       */
    SIGPROC_LP  = 1,    /* low-pass,  -3 dB at fs/8 ("sigproc on" = this)   */
    SIGPROC_HP  = 2,    /* high-pass, -3 dB at fs/8, output + 2048          */
    SIGPROC_BP  = 3     /* band-pass, centre fs/8, one octave, output + 2048 */
} sigproc_filter_t;

void sigproc_set_filter(sigproc_filter_t f);     /* restarts the filter state */
sigproc_filter_t sigproc_filter(void);
const char *sigproc_filter_name(sigproc_filter_t f);  /* "off" "lp" "hp" "bp" */

/* The Goertzel's result for the last block it saw. */
typedef struct {
    uint32_t amp;       /* the fs/16 tone's amplitude, LSB                  */
    uint32_t rms;       /* the block's rms around its mean, LSB             */
    uint32_t share_pm;  /* the tone's share of that power, per mille        */
    uint32_t thr;       /* the detection threshold, LSB                     */
    uint32_t detected;  /* 1: amp >= thr                                    */
    uint32_t seq;       /* the block it was computed on (info->seq)         */
    uint32_t valid;     /* 0: no block since it was switched on             */
} sigproc_gz_t;

void sigproc_set_goertzel(bool on);
bool sigproc_goertzel_on(void);
void sigproc_set_threshold(uint32_t lsb);         /* default 100 LSB */
void sigproc_goertzel_get(sigproc_gz_t *out);

/* The stream's sample rate, set by acquisition.c when a stream starts.
 * The filters and the Goertzel do not need it (their coefficients are
 * fractions of fs); it is passed on to sigproc_app_set_fs(). */
void sigproc_set_fs(float fs_hz);

/* ---- The application's own processing (02.10.2026): a project adds its
 * own analysis of the blocks without changing this file, by defining the
 * functions below in a file of its own; the weak defaults in sigproc.c do
 * nothing. sigproc_app_block() gets every half's samples as they came in,
 * before the filter (which works in place), under the rules at the top of
 * this header - main loop, within one half period, no console output - and
 * reads them only. The console's "sigproc" command hands a sub-command it
 * does not know to sigproc_app_cmd() and appends sigproc_app_status() to
 * its status (console.h); "stream grab" appends gui_link_app_fields() to
 * the GRAB header (gui_link.h). */
bool sigproc_app_active(void);          /* true: sigproc_app_block() wanted */
void sigproc_app_set_fs(float fs_hz);   /* from sigproc_set_fs()            */
void sigproc_app_block(const uint16_t *x, uint32_t n, const sigproc_info_t *info);

/* true when a filter, the Goertzel or the application's processing is on:
 * capture.c then calls sigproc_block() (capture_sigproc_enable(), set by
 * the console). */
bool sigproc_active(void);

#endif

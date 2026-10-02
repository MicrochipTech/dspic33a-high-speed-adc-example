/*
 * impact.h - the impact counter (CNT, 02.10.2026, docs/IMPLEMENTATION-PLAN.md):
 * a damped Goertzel resonator at the plate's ring frequency, run per sample
 * on the input before the filter, and a detector that counts once when the
 * resonator's magnitude rises above `thr` and re-arms relative to the peak
 * (impact.c says how). Its state carries across halves; a reset starts
 * count and time over. Needs the stream's sample rate: sigproc_set_fs(),
 * called by acquisition.c when a stream starts, hands it on through
 * sigproc_app_set_fs().
 *
 * It hooks into the example through sigproc.h's application hooks and
 * needs no change in the core; this header is the counter's own interface,
 * for impact.c's console part and the host test.
 */
#ifndef IMPACT_H
#define IMPACT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t on;        /* 1: counting                                       */
    uint32_t f_hz;      /* the resonator's frequency                         */
    uint32_t tau_us;    /* its time constant (D = exp(-1 / (tau fs)))        */
    uint32_t thr;       /* count above this magnitude, LSB of tone amplitude */
    uint32_t count;     /* impacts since the reset                           */
    uint32_t rate;      /* impacts per second over the time seen since then  */
    uint32_t ms;        /* that time, ms (the samples seen / fs)             */
    uint32_t missed;    /* halves the processing never saw since the reset   */
    uint32_t peak;      /* the largest magnitude since the last peak read, LSB */
    uint32_t fs_hz;     /* the sample rate it is set up for (0: none yet)    */
} impact_t;

void impact_enable(bool on);
bool impact_on(void);
/* f 1000..fs/2.5 (checked against the running fs when it is known), tau
 * 2..5000 us, thr 1..4095. false: refused, nothing changed. Restarts the
 * resonator; the count goes on. */
bool impact_config(uint32_t f_hz, uint32_t tau_us, uint32_t thr);
void impact_reset(void);
/* clear_peak: the grab reads the peak since the previous grab; the
 * console leaves it. */
void impact_get(impact_t *out, bool clear_peak);

#endif

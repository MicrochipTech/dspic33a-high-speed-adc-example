/*
 * capture.h - the measurement of the ADC/DMA example (capture.c)
 *
 * Everything the console can read or control is here; the console never
 * touches ADC or DMA registers itself.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The buffer is allocated at this maximum; the length in use is set at
 * run time (capture_set_half_len, "buf" command) and defaults to the
 * maximum, so nothing changes unless someone asks. */
#define SAMPLES_PER_HALF_MAX  1024u
#define SAMPLES_PER_BUF_MAX   (2u * SAMPLES_PER_HALF_MAX)
#define SAMPLES_PER_HALF_MIN  16u

/* ---- Measurement state (defined in capture.c) ---- */
/* The sample buffer itself is private to capture.c (it sits in a struct
 * with guard words behind it); readers use capture_completed_half(). */

extern volatile uint32_t blocks_done;    /* completed buffer halves        */
extern volatile uint32_t dma_overrun;    /* DMA triggered while busy       */
extern volatile uint32_t dma_addr_err;   /* access outside DMALOW..DMAHIGH */
extern volatile uint32_t dma_bus_err;    /* bus write error (BWERR)        */
extern volatile uint32_t late_service;   /* ISR more than one half late    */
extern volatile uint32_t proc_missed;    /* main() skipped a completed half*/
extern volatile uint16_t last_sample;    /* last value of the completed half */
extern volatile uint32_t ready_half;     /* 0 or 1: which half is complete */
extern volatile uint32_t selftest_mean;  /* last self-test result (~3840)  */
extern volatile int32_t  proc_result;    /* output of process_buffer()     */

/* DMA channel 0 from the ADC result into buf[], HALF/DONE interrupts
 * enabled. Nothing transfers until capture_start(). */
void capture_init(void);
/* Stream off hard: DMA interrupt masked, channel disabled. For fail()
 * and the trap handlers; nothing restarts after this. */
void capture_halt(void);

/* Start the burst stream; a no-op while it runs. */
void capture_start(void);
/* Everything off: stream stopped, ADC core down, CLKGEN6 off. No
 * conversion, no DMA event, no interrupt from the ADC side - the console
 * has the CPU to itself. capture_start() brings clock and core back with
 * the pacing and period they had. capture_powered() says which state. */
void capture_shutdown(void);
bool capture_powered(void);

/* The defined idle state every test starts from: stream stopped, the
 * burst in flight finished (or, if it never ends, aborted by taking the
 * core down and up), stale ADC events cleared, the DMA channel taken
 * down (dma0_deinit), ready_half 0. capture_start() sets the channel up
 * again from scratch (dma0_init) before the first transfer. So every
 * test ends with the DMA off and starts with a freshly initialised one;
 * no test inherits the buffer position or the leftovers of the one
 * before - a concern in particular for the single-conversion source,
 * whose stop (timer off) can fall anywhere in the buffer. Returns
 * whether the stream was running, for the caller to restart it. */
bool capture_settle(void);

/* Samples per buffer half in use. Changing it: only with the stream
 * stopped; the call settles (DMA down), the next capture_start() sets
 * ADC burst length, DMA block and guard words up for the new size.
 * 16..SAMPLES_PER_HALF_MAX. False if out of range or while running. */
uint32_t capture_half_len(void);
bool     capture_set_half_len(uint32_t n);

/* Switch to another ADC core (1..5) with input pinsel and sample time
 * samc: stream stopped, core down, table row switched, adc_init(), DMA
 * re-armed on that core's trigger and result register, pacing back to
 * the repeat timer, clock divider back to 1. Leaves the core powered and
 * idle. False for a bad core number. */
bool capture_select_core(uint8_t core, uint8_t pinsel, uint8_t samc);
/* Let the current burst finish and do not restart it. */
void capture_stop(void);
bool capture_running(void);
/* True from the burst trigger until the DMA DONE event. */
bool capture_burst_active(void);

/* Change input pin and sample time. Applied by the ISR between two bursts,
 * when the channel is idle; returns false for out-of-range arguments
 * (PINSEL 0..15, SAMC 0..31). */
bool    capture_set_input(uint8_t pinsel, uint8_t samc);
uint8_t capture_pinsel(void);
uint8_t capture_samc(void);

/* ---- Pacing: what sets the sample rate ----
 * ADC_TRG2_REPEAT: the ADC's repeat timer, period in TAD (12.5 ns), 2..63.
 * ADC_TRG2_SCCP1:  SCCP1 timer, period in ticks of 10 ns, 2..65535.
 * ADC_TRG2_B2B:    back-to-back, no period, as fast as the converter goes.
 * ADC_PACE_CLKDIV: back-to-back conversions, the rate set by the ADC clock
 *                  divider (clock.c); period = divide ratio of the 320 MHz
 *                  clock in hundredths, 100..1000 (40 ... 4 MSPS).
 * ADC_PACE_SINGLE: one conversion per SCCP1 trigger (Single Conversion
 *                  mode, TRG1SRC = SCCP1), period in ticks of 10 ns; no
 *                  burst, no restart. Microchip's 40 MSPS example's way.
 * capture_set_pacing() switches source and default period between two
 * bursts; capture_set_period() sets the period in the active source's
 * unit, applied between two bursts; false for out-of-range or for B2B.
 * capture_nominal_ksps() is the rate the active source should deliver at
 * a period; 0 for B2B. capture_sweep_periods() is the list the sweep
 * steps for the active source (slowest first). */
bool     capture_set_pacing(uint8_t trg2src);
uint8_t  capture_pacing(void);
const char *capture_pacing_name(void);
bool     capture_set_period(uint32_t period);
uint32_t capture_period(void);
uint32_t capture_nominal_ksps(uint32_t period);
const uint32_t *capture_sweep_periods(uint32_t *count);
/* A second list for a second sweep pass, or NULL (count 0): for the clock
 * divider the fractional ratios, after the even ones. */
const uint32_t *capture_sweep_periods2(uint32_t *count);

/* Sample the ADC's internal 15/16 * VDD reference (ANx6) for a few halves
 * and compare the mean against the expected window. Blocking, bounded.
 * Returns 0 on success, 6 if no data arrived, 7 if the mean is outside the
 * window, 8 if the DMA channel switched itself off. The mean is stored in
 * selftest_mean and returned through *mean if non-NULL. Restores the
 * previous input afterwards. */
uint32_t capture_selftest(uint32_t *mean);

/* Rate self-test for the active pacing: a few hundred halves at two
 * periods a factor 4 apart, the delivered rate measured against Timer1
 * and judged against the nominal one (10 %) and against each other
 * (3..5x) - the failure seen on the board was a rate that did not move.
 * Blocking, bounded, prints its numbers. Returns 0, or 6/8 from the
 * waits, or 12. Restores the previous period. For B2B it only measures
 * and prints (nothing to judge, returns 0). Skipped in the simulator. */
uint32_t capture_ratetest(void);

/* Choose the pacing at boot per ADC_PACING (board.h). AUTO: run the rate
 * test on every candidate - one conversion per SCCP1 trigger, ADC clock
 * divider, repeat timer, SCCP1 as burst trigger, back-to-back - print
 * each result, then take the first that passed, back-to-back if none.
 * A fixed source is tested once and the boot stops with its code if it
 * fails. Returns 0 or that code. */
uint32_t capture_autopace(void);

/* Process the completed half if a new one arrived; returns true if it did.
 * Called from the main loop and from the console's yield hook, so that
 * the measurement keeps running while a long console reply drains. */
bool capture_service(void);

/* Pointer to the half that completed last. */
const volatile uint16_t *capture_completed_half(void);

void counters_clear(void);

/* The counters as "name: value" lines (part of regs_dump()). */
void capture_regs_dump(void);

#endif /* CAPTURE_H */

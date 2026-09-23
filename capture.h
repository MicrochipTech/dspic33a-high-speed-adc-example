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

#define SAMPLES_PER_HALF  1024u
#define SAMPLES_PER_BUF   (2u * SAMPLES_PER_HALF)

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

/* Sample the ADC's internal 15/16 * VDD reference (ANx6) for a few halves
 * and compare the mean against the expected window. Blocking, bounded.
 * Returns 0 on success, 6 if no data arrived, 7 if the mean is outside the
 * window, 8 if the DMA channel switched itself off. The mean is stored in
 * selftest_mean and returned through *mean if non-NULL. Restores the
 * previous input afterwards. */
uint32_t capture_selftest(uint32_t *mean);

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

/*
 * adc_dma_40msps.h
 *
 * Interface between the measurement core (adc_dma_40msps.c) and the
 * console (cli.c). Everything the console can read or control is here;
 * the console never touches ADC or DMA registers itself.
 */
#ifndef ADC_DMA_40MSPS_H
#define ADC_DMA_40MSPS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Compile-time configuration (documented in README) ---- */

/* ADC core and input. EV74H48A defaults: ADC3, AD3AN5 = mikroBUS A pin AN
 * (device pin RA0). The on-board potentiometer is AD5AN0 (RA7): set
 * ADC_INSTANCE 5, ADC_PINSEL 0 and a longer sample time for that. */
#ifndef ADC_INSTANCE
#define ADC_INSTANCE      3
#endif
#ifndef ADC_PINSEL
#define ADC_PINSEL        5u
#endif
#ifndef ADC_SAMC
#define ADC_SAMC          0u      /* 0.5 TAD, 40 MSPS at 320 MHz input clock */
#endif

#define SAMPLES_PER_HALF  1024u
#define SAMPLES_PER_BUF   (2u * SAMPLES_PER_HALF)

/* Bound for every hardware wait loop, in loop iterations. A step that
 * needs longer than this has failed; fail() then reports which one. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define WAIT_LIMIT        20000u     /* simulator runs at ~1/80 real time */
#else
#define WAIT_LIMIT        2000000u
#endif

/* ---- Measurement state (defined in adc_dma_40msps.c) ---- */
extern volatile uint16_t buf[SAMPLES_PER_BUF];

extern volatile uint32_t blocks_done;    /* completed buffer halves        */
extern volatile uint32_t dma_overrun;    /* DMA triggered while busy       */
extern volatile uint32_t dma_addr_err;   /* access outside DMALOW..DMAHIGH */
extern volatile uint32_t dma_bus_err;    /* bus write error (BWERR)        */
extern volatile uint32_t late_service;   /* ISR more than one half late    */
extern volatile uint32_t proc_missed;    /* main() skipped a completed half*/
extern volatile uint16_t last_sample;    /* last value of the completed half */
extern volatile uint32_t ready_half;     /* 0 or 1: which half is complete */
extern volatile uint32_t selftest_mean;  /* last self-test result (~3840)  */
extern volatile uint32_t fail_code;      /* != 0: stopped, see fail()      */

/* Start-up progress and the last trap, in persistent RAM so both survive
 * the reset that an unhandled trap would otherwise hide. boot_mark() is
 * called at each step of main(); _DefaultInterrupt() prints the lot and
 * blinks code 9. trap_seen != 0 at start-up means the previous run hit a
 * trap - main() reports that before doing anything else. */
extern volatile uint32_t boot_stage;
extern volatile uint32_t trap_seen;
extern volatile uint32_t trap_vec;
extern volatile uint32_t trap_stage;
void boot_mark(uint32_t stage);
extern volatile int32_t  proc_result;    /* output of process_buffer()     */

/* ---- Initialisation (implemented in adc_dma_40msps.c), in this order ---- */

/* LED0 off and configured as output. */
void led_init(void);
/* FRC -> PLL1 320 MHz -> CLKGEN6 (ADC), PLL2 200 MHz -> CLKGEN1 (system).
 * Every wait is bounded; a step that fails stops in fail(1..4). */
void clock_init(void);
/* ADC core ADC_INSTANCE, channel 0, Integration mode, CNT = SAMPLES_PER_BUF.
 * Stops in fail(5) if the core never reports ready. */
void adc_init(uint8_t pinsel, uint8_t samc);
/* DMA0 from the ADC result into buf[], HALF/DONE interrupts enabled.
 * Nothing transfers until capture_start(). */
void dma0_init(void);
/* False once the DMA switched itself off (address fault). */
bool dma0_enabled(void);

/* ---- Control (implemented in adc_dma_40msps.c) ---- */

/* Start the burst stream; a no-op while it runs. */
void capture_start(void);
/* Let the current burst finish and do not restart it. */
void capture_stop(void);
bool capture_running(void);

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

/* LED0: 0 = off, 1 = on, 2 = automatic (heartbeat while running). */
void led_mode(uint8_t mode);
uint8_t led_get_mode(void);

/* Stop with a blink code (never returns). */
void fail(uint32_t code);

/* ---- Register dump (implemented in adc_dma_40msps.c) ---- */
/* Clock, ADC, DMA, interrupt and UART registers as "name: 0x........"
 * lines on the console. Printed by fail() and by the "regs" command. */
void regs_dump(void);

/* ---- Console (implemented in cli.c) ---- */

/* UART2 up on the FRC, before the clocks are touched. From here on
 * console_puts() works. */
void console_early_init(void);
/* After clock_init(): baud generator on the PLL clock, parser, banner,
 * receive interrupt. */
void cli_init(void);
/* Baud generator re-matched to the current CPU clock; used by fail(). */
void console_sync_baud(void);
/* Re-establish pins, PPS and UART from scratch, assuming nothing about
 * the current state. Used by the trap handler, which cannot rely on the
 * console still being intact. */
void console_force_up(void);
/* Blocking trace output, safe from main() and from fail(). */
void console_puts(const char *s);
void console_kv(const char *key, uint32_t v);        /* "key: 123"        */
void console_kv_hex(const char *key, uint32_t v);    /* "key: 0x00000123" */
/* One line with every counter, for the periodic trace from main(). */
void console_status_line(void);

#endif /* ADC_DMA_40MSPS_H */

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
 * the divide ratio it had. capture_powered() says which state. */
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
 * re-armed on that core's trigger and result register. The ADC clock
 * divider is left as it is. Leaves the core powered and idle. False for a bad core number. */
bool capture_select_core(uint8_t core, uint8_t pinsel, uint8_t samc);
/* Let the current burst finish and do not restart it. */
void capture_stop(void);
bool capture_running(void);
/* True if the overrun brake fired during the last measurement: the
 * handler saw more overruns than any usable rate can produce, masked
 * its own interrupt and took the channel down, so that the storm could
 * not lock the CPU out of the main loop. The rate that caused it is
 * unusable by definition. Cleared by counters_clear(). */
bool capture_overrun_aborted(void);
/* True from the burst trigger until the DMA DONE event. */
bool capture_burst_active(void);

/* Change input pin and sample time. Applied by the ISR between two bursts,
 * when the channel is idle; returns false for out-of-range arguments
 * (PINSEL 0..15, SAMC 0..31). */
bool    capture_set_input(uint8_t pinsel, uint8_t samc);
uint8_t capture_pinsel(void);
uint8_t capture_samc(void);

/* ---- The ADC clock: what sets the sample rate ----
 *
 * The conversions run back-to-back, so the rate is the ADC clock divided
 * by the eight clocks one conversion takes. The clock comes from PLL1
 * through CLKGEN6, and PLL1 feeds nothing else (the CPU is on PLL2).
 *
 * Two knobs exist and only one of them works:
 *   capture_set_pll(p1, p2)  PLL1's output dividers, 1600 MHz / (p1*p2),
 *                            p1 >= p2, both 1..7: 320 down to 32.65 MHz,
 *                            i.e. 40 down to 4.08 MSPS, and 5/5 = 8 MSPS.
 *                            THIS is the rate control.
 *   capture_set_clkdiv(r)    the CLKGEN6 divide ratio in hundredths. It
 *                            arrives in the register and is confirmed by
 *                            DIVSWEN and CLKRDY - and does not change the
 *                            conversion rate (HARDWARE-LOG runs 8 and 9).
 *                            Kept for the record and for the "clk"
 *                            command; do not build on it.
 * Both return CLKDIV_OK or the step that failed (clock.h), and both do
 * the switch in the boot order: DMA channel down, ADC core off, clock
 * changed, core on, DMA set up from scratch on the next start.
 *
 * capture_nominal_ksps() is what the hardware is set up for, read back
 * from the clock registers - not what was asked for. */
struct pll_step { uint8_t p1, p2; };
uint32_t capture_set_pll(uint32_t p1, uint32_t p2);
/* The same switch, but addressed by a rate instead of by divider
 * settings: the closest combination of PLL1 output dividers and PLLFBDIV
 * is chosen (clock.h), so the caller says 8000 and gets 8000. *got_ksps
 * is what the hardware will deliver - always read it, the wish is not
 * always reachable exactly. */
uint32_t capture_set_rate(uint32_t want_ksps, uint32_t *got_ksps);
uint32_t capture_set_clkdiv(uint32_t ratio_h);
uint32_t capture_clkdiv(void);
uint32_t capture_clkdiv_wanted(void);
uint32_t capture_nominal_ksps(uint32_t ignored);
/* The rate ladder, slowest first. */
const struct pll_step *capture_sweep_steps(uint32_t *count);

/* ---- The variant matrix ----
 *
 * Every documented way to set the sample rate on this device, as
 * something the board can be asked to try one after another. The point
 * is not elegance: four of these were tried before and reported as "does
 * not work", and for three of them the reason turned out to be in our own
 * code (see sccp.h). Since the documentation has been wrong twice, the
 * board decides.
 *
 * capture_select_variant() takes the target rate in kSPS and translates
 * it into that variant's registers - a PLL pair, an SCCP period, an
 * RPTCNT, an accumulation count. It leaves the chain configured and idle;
 * capture_oneshot() then runs it. False if the variant could not be set
 * up at all (a clock that did not come, a period out of range).
 *
 * capture_trigger_period_ns() is 0 for the untriggered variants and the
 * exact trigger period for the others. With it, one buffer and the window
 * length from Timer1, the number of DMA transfers per trigger can be
 * computed - which is the direct measurement of the issue Microchip
 * acknowledges for this silicon: "ADC triggers for DMA on this device
 * have an issue. A few transfers are possible per one trigger." */
typedef enum {
    CAP_VAR_B2B = 0,          /* back-to-back, rate from PLL1           */
    CAP_VAR_SCCP_T_PER,       /* SCCP1 timer + special event, periph clk */
    CAP_VAR_SCCP_T_G13,       /* SCCP1 timer + special event, CLKGEN13   */
    CAP_VAR_SCCP_OC_PER,      /* SCCP1 output compare, periph clk        */
    CAP_VAR_SCCP_OC_G13,      /* SCCP1 output compare, CLKGEN13          */
    CAP_VAR_SCCP_OLD,         /* the combination that failed in runs 5-7 */
    CAP_VAR_SCCP_TRG2,        /* SCCP1 as TRG2 inside an Integration burst */
    CAP_VAR_RPTCNT,           /* the ADC repeat timer                    */
    CAP_VAR_OVERSAMPLE,       /* MODE 3, ACCNUM divides the event rate   */
    CAP_VAR_CLKDIV,           /* the CLKGEN6 divider                     */
    CAP_VAR_COUNT
} capture_variant_t;

bool        capture_select_variant(capture_variant_t v, uint32_t want_ksps);
const char *capture_variant_name(capture_variant_t v);
uint32_t    capture_variant_ksps(void);        /* what it should deliver */
uint32_t    capture_trigger_period_ns(void);   /* 0 = untriggered        */
void        capture_variant_regs(void);        /* the defining registers */

/* Sample the ADC's internal 15/16 * VDD reference (ANx6) for a few halves
 * and compare the mean against the expected window. Blocking, bounded.
 * Returns 0 on success, 6 if no data arrived, 7 if the mean is outside the
 * window, 8 if the DMA channel switched itself off. The mean is stored in
 * selftest_mean and returned through *mean if non-NULL. Restores the
 * previous input afterwards. */
uint32_t capture_selftest(uint32_t *mean);

/* Switch CLKGEN6 off and try to convert anyway: the control experiment
 * for "is the ADC really clocked from CLKGEN6?". 0 means halves still
 * arrived with the generator off, anything else that nothing did. The
 * generator and the ADC core are restored either way. Blocking, bounded.
 * In the simulator it returns 6 without doing anything. */
uint32_t capture_clkoff_probe(uint32_t halves);

/* Run `halves` halves at whatever the divider is set to and return the
 * delivered rate in ksps, measured against Timer1. Blocking, bounded,
 * prints nothing - the caller judges and reports. Returns 0, or 6/8 from
 * the waits. In the simulator it returns 0 with ksps 0. */
uint32_t capture_measure_rate(uint32_t halves, uint32_t *ksps);

/* Process the completed half if a new one arrived; returns true if it did.
 * Called from the main loop and from the console's yield hook, so that
 * the measurement keeps running while a long console reply drains. */
bool capture_service(void);

/* Fill the buffer exactly once and stop, the stop decided in the DMA
 * interrupt. Afterwards the whole buffer - 2 * capture_half_len()
 * samples from capture_buffer() - is one contiguous window that nothing
 * is writing any more. This is the only way to look at the data at a
 * rate where the main loop runs tens of milliseconds behind the DMA.
 * Returns 0, or 6/8 from the wait. */
uint32_t capture_oneshot(void);
/* The whole buffer. Only meaningful with the stream stopped. */
const volatile uint16_t *capture_buffer(void);

/* Pointer to the half that completed last. */
const volatile uint16_t *capture_completed_half(void);

void counters_clear(void);

/* The counters as "name: value" lines (part of regs_dump()). */
void capture_regs_dump(void);

#endif /* CAPTURE_H */

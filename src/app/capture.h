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
/* capture_process_bench()/_selftest()/_clkoff_probe()/_measure_rate()/
 * _oneshot()/_oneshot_n() moved to src/meter/meter.c (P9.3, 27.09.2026).
 * Declared in meter.h, pulled in here so every existing caller of
 * capture.h keeps working unchanged - the smaller diff over adding
 * #include "meter.h" to cli.c/bench.c/gui_link.c/chaintest.c/dactest.c
 * individually. */
#include "meter.h"
/* capture_set_pll()/_set_rate()/_set_clkdiv() and the variant matrix
 * (capture_select_variant() and its reporting functions) moved to
 * src/app/acquisition.c (P9.4, 27.09.2026), together with chaintest.c's
 * chain_stream_*() - the standing stream is acquisition, not a test.
 * Declared in acquisition.h, pulled in here the same way meter.h is. */
#include "acquisition.h"

/* The buffer is allocated at this maximum; the length in use is set at
 * run time (capture_set_half_len, "buf" command) and defaults to the
 * maximum, so nothing changes unless someone asks.
 *
 * Two ping-pong pairs since 01.10.2026 (2 x 2048 = one pair the same
 * morning, 2 x 1024 before): the 8 KB hold pair A (samples 0..2H-1) and
 * pair B (2H..4H-1), H = the half length in use. The triggered stream
 * runs on one pair; for a "stream grab" it moves on to the other without
 * stopping and the first is sent (capture_pair_freeze(), dma.c's pair
 * mode). SAMPLES_PER_BUF_MAX is one pair - the most a block, a burst, a
 * grab or "blk" covers; SAMPLES_PER_ALLOC is what the buffer holds. */
#define SAMPLES_PER_HALF_MAX  1024u
#define SAMPLES_PER_BUF_MAX   (2u * SAMPLES_PER_HALF_MAX)
#define SAMPLES_PER_ALLOC     (2u * SAMPLES_PER_BUF_MAX)
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
/* Why these exist: see capture.c. They separate "one conversion causes
 * several DMA transfers" from "the handler counts the same event twice". */
extern volatile uint32_t isr_entries;    /* calls of dma0_event()          */
extern volatile uint32_t half_events;    /* HALF seen set                  */
extern volatile uint32_t done_events;    /* DONE seen set                  */
extern volatile uint32_t burst_starts;   /* start_burst() calls - blocks_done
                                          * must be exactly twice this      */

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
 * 16..SAMPLES_PER_HALF_MAX, even. False if out of range, odd or while
 * running. */
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
/* The burst stream should run but its DMA channel is off - read in one
 * go, safe against a console command changing the state in between
 * (main()'s fail(8) test; capture.c says why it matters). */
bool capture_stream_lost(void);
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
 * capture_set_pll()/_set_rate()/_set_clkdiv() - the two knobs, only one of
 * which works - moved to src/app/acquisition.c (P9.4, 27.09.2026),
 * declared in acquisition.h, included above. What stays here reads back
 * what the hardware is set up for rather than changing it:
 *
 * capture_nominal_ksps() is what the hardware is set up for, read back
 * from the clock registers - not what was asked for. */
struct pll_step { uint8_t p1, p2; };
uint32_t capture_clkdiv(void);
uint32_t capture_clkdiv_wanted(void);
uint32_t capture_nominal_ksps(uint32_t ignored);
/* The rate ladder, slowest first - acquisition.c's capture_select_
 * variant() reads it too, through this same public accessor. */
const struct pll_step *capture_sweep_steps(uint32_t *count);

/* The variant matrix (capture_variant_t, capture_select_variant() and its
 * reporting functions) moved to src/app/acquisition.c with the rate
 * setters above (P9.4, 27.09.2026); declared in acquisition.h, included
 * above. */

/* Process the completed half if a new one arrived; returns true if it did.
 * Called from the main loop, from the bench's and the chain test's own
 * streaming loops, and from "stream grab" while it waits for the pair
 * move (capture_pair_freeze()).
 * NOT from the console's yield hook, as this comment said until
 * 01.10.2026 - console_yield() (cli.c) only watches for Ctrl+C. While a
 * console command runs (inside the receive interrupt) the main loop does
 * not run; since 01.10.2026 its output goes into uart.c's transmit ring
 * and the command returns long before the text is on the line. */
bool capture_service(void);

/* The signal processing (sigproc.c, sigproc.h): sigproc_block() is called
 * for each completed half only while it is switched on - console command
 * "sigproc on|off", off after reset. busy: capture_service() is running
 * with it on (cli.c's uart_rx_hook() holds bytes back meanwhile). */
void capture_sigproc_enable(bool on);
bool capture_sigproc_enabled(void);
bool capture_sigproc_busy(void);

/* capture_oneshot()/capture_oneshot_n(): meter.h (P9.3, 27.09.2026). */
/* Timer1 ticks of the last one-shot, the BURST ALONE - the DMA channel
 * being taken down and set up again costs a fixed 11.3 us and used to sit
 * inside the measured window, which made every rate read low (run 14).
 * Use this instead of timing around capture_oneshot(). */
uint32_t capture_oneshot_ticks(void);
/* The whole buffer. Only meaningful with the stream stopped. */
const volatile uint16_t *capture_buffer(void);

/* ---- The triggered stream: the chain of the example ----
 *
 * SCCP1 on CLKGEN13 paces channel 0 in Single Conversion mode (the
 * caller has put the ADC in that mode), one DMA transfer per conversion,
 * Repeated One-Shot, no burst and no restart. See capture.c.
 *
 * capture_chain_start()  settle, counters cleared, DMA armed (source
 *                        CH0DATA if src_data, else CH0RES), then SCCP1
 *                        started last with period `ticks` of its clock
 *                        in sccp_mode (sccp_mode_t). stop_after_done > 0:
 *                        the DMA ISR stops the trigger at that DONE, and
 *                        the buffer holds exactly the last block.
 * capture_chain_wait()   until that stop, bounded in Timer1 ticks: 0, 6
 *                        (timeout) or 8 (DMA switched itself off).
 * capture_chain_stop()   trigger off first, the last transfer awaited,
 *                        then settled. Returns the transfers counted.
 * capture_transfers()    transfers so far: DONE blocks and DMA0CNT.
 * capture_chain_window_ticks()  Timer1 from trigger start to stop.
 * capture_fill()         the whole buffer to one value (a sentinel the
 *                        ADC cannot produce), only with the DMA idle. */
bool     capture_chain_start(uint32_t ticks, uint32_t sccp_mode,
                             uint32_t stop_after_done, bool src_data);
bool     capture_chain_active(void);
uint32_t capture_chain_wait(uint32_t max_ticks);
uint64_t capture_chain_stop(void);
uint64_t capture_transfers(void);
/* The guard words behind the buffer intact? (capture_service() stops in
 * fail(11) instead; this one only reports.) */
bool     capture_guard_ok(void);
uint32_t capture_chain_window_ticks(void);
void     capture_fill(uint16_t v);

/* Halt / resume an ALREADY RUNNING triggered stream without tearing the
 * DMA channel down (capture_chain_start/_stop set the whole chain up or
 * take it fully apart; these two are for a stream meant to keep running
 * with brief, repeated pauses - the GUI's halt/grab/restart cycle,
 * chain_stream_grab_begin/_end in chaintest.c).
 *
 * capture_chain_halt(): the trigger stops first (ANALYSIS.md C.10 point
 * 4), a short bounded wait lets the one conversion already in flight
 * land, and nothing converts after that - so the half that completed
 * last (capture_completed_half(), capture_half_len() samples) stands
 * still, contiguous, for as long as the caller needs. The DMA channel,
 * the ADC core and the clock tree are left exactly as they were; only
 * the trigger is off. False if no chain stream is active (already
 * halted, or none was ever started) - nothing is touched in that case.
 *
 * capture_chain_resume(): restarts the SAME trigger (same period, same
 * mode) capture_chain_halt() paused, so the ping-pong buffer continues
 * where it left off. False if the trigger could not be restarted; the
 * stream is then left exactly as capture_chain_halt() leaves it - not
 * running, everything else intact - so the caller can report it and the
 * next attempt starts from a known state rather than from something
 * half torn down. */
bool capture_chain_halt(void);
bool capture_chain_resume(void);
/* "stream grab" since 01.10.2026: the triggered stream moves on to the
 * other ping-pong pair and the pair just completed (ping then pong,
 * 2 * half_len samples from *from) stands still until released - the
 * stream never stops (capture.c). Waits at most max_ticks (timebase),
 * keeping capture_service() going meanwhile. */
bool capture_pair_freeze(uint32_t max_ticks, const volatile uint16_t **win,
                         uint32_t *n, uint32_t *from);
void capture_pair_release(void);
/* capture_process_bench(): meter.h (P9.3, 27.09.2026). */
/* Processing cost of a half in Timer1 ticks, since counters_clear(). */
extern volatile uint32_t proc_ticks_max;
extern volatile uint32_t proc_ticks_sum;
extern volatile uint32_t proc_count;

/* Pointer to the half that completed last. */
const volatile uint16_t *capture_completed_half(void);

void counters_clear(void);

/* The counters as "name: value" lines (part of regs_dump()). */
void capture_regs_dump(void);

#endif /* CAPTURE_H */

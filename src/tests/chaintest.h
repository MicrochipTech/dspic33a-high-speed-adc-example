/*
 * chaintest.h - the chain test: SCCP1 -> ADC -> DMA -> ping-pong -> CPU
 * (chaintest.c, plan in docs/CHAIN-TEST-PLAN.md)
 */
#ifndef CHAINTEST_H
#define CHAINTEST_H

#include <stdint.h>
#include <stdbool.h>

/* Stages first..last (0..9) of the chain test, then the summary and
 * @END. chain_all(0, 9) is "chain all", chain_all(n, 9) "chain from n",
 * chain_all(n, n) one stage. The clock tree and core 5 are always set up
 * first, quietly if stage 0 is not part of the run. Blocking, about a
 * minute for the whole run. Restores the boot configuration at the end. */
void chain_all(uint32_t first, uint32_t last);

/* The chain as the example would run it: set up, `seconds` of triggered
 * stream at about `ksps` (the nearest 160 MHz / N), the CPU processing
 * every half, one status line per second (printed afterwards, so that
 * printing does not disturb the stream), a verdict. */
void chain_run(uint32_t ksps, uint32_t seconds);

/* The chain as a standing stream, the way the example runs it:
 * chain_stream_on() sets it up at about `ksps` (the nearest 160 MHz / N,
 * 1..40000) with the DAC triangle on RA8 as the signal and returns; main()
 * then processes every half. chain_stream_off() stops it and restores the
 * boot configuration; chain_all(), chain_run() and the old tests call it
 * first. chain_stream_state(): rate, transfers since the start, free CPU
 * cycles per sample; false if no stream is on, or if it stopped itself
 * (overrun brake) - chain_streaming() still says one was started. */
bool chain_stream_on(uint32_t ksps);
/* The same with the input chosen: ADC core 1..5, PINSEL 0..15, SAMC 0..31,
 * and test_signal = whether DAC2's triangle is started as the signal (it
 * is on RA8 = AD5AN3 and on UREF = ANn7). Without it the DAC is left as it
 * is, and the grab frame says slp=0. chain_stream_on(ksps) is core 5,
 * PINSEL 3 (RA8), SAMC 0, with the triangle. */
bool chain_stream_on_input(uint32_t ksps, uint8_t core, uint8_t pinsel, uint8_t samc,
                           bool test_signal);
void chain_stream_off(void);
bool chain_streaming(void);
bool chain_stream_state(uint32_t *ksps, uint64_t *transfers, uint32_t *free_cyc);

/* One halt / grab / resume cycle of the standing stream ("stream grab",
 * cli.c), for the GUI: with chain_stream_on() already running,
 * chain_stream_grab_begin() halts the trigger (capture_chain_halt():
 * trigger first) and hands back a pointer to the half that stood still -
 * contiguous, win_len samples, at offset `from` in the raw buffer (0 or
 * capture_half_len()) - together with the counters since the PREVIOUS
 * grab (or since chain_stream_on(), for the first one: "per-cycle"
 * values, not the running total - a colleague watching the GUI wants to
 * know what happened in the window just shown) and the DAC triangle
 * setting the GUI's model needs. False if no stream is on, or the halt
 * itself failed (the overrun brake firing between two grabs, for
 * instance) - chain_stream_off() has then already been called, so
 * chain_streaming() reports it and the caller is expected to send
 * "stream on" again. The window is only valid until
 * chain_stream_grab_end() is called - nothing else may run in between.
 *
 * chain_stream_grab_end() restarts the SAME trigger (same rate) that was
 * paused; call it once after every successful begin, whether or not the
 * transfer in between went out whole. False means the restart itself
 * failed, in which case the stream is left off, same as above. */
typedef struct {
    uint32_t ksps;
    const volatile uint16_t *win;
    uint32_t win_len;
    uint32_t from;
    uint32_t overrun, late, missed, halves;
    uint32_t transfers;
    uint16_t slpdat;
    uint32_t dac_hz;
} chain_grab_t;

bool chain_stream_grab_begin(chain_grab_t *g);
bool chain_stream_grab_end(void);

#endif /* CHAINTEST_H */

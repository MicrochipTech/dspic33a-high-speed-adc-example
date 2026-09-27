/*
 * acquisition.h - choosing and running the acquisition (acquisition.c)
 *
 * Moved out of capture.c and chaintest.c on 27.09.2026 (P9.4,
 * docs/IMPLEMENTATION-PLAN.md):
 *
 *   - capture.c's rate setters (capture_set_pll(), capture_set_rate(),
 *     capture_set_clkdiv()) and the variant matrix (capture_select_
 *     variant() and its reporting functions) - what to run the ADC at
 *     and how;
 *   - chaintest.c's chain_stream_*() functions - the standing stream the
 *     GUI drives ("stream on"/"stream grab"), which is acquisition, not a
 *     test, even though it grew inside the chain test file first.
 *
 * Declared here rather than in capture.h/chaintest.h so that this file,
 * not capture.c/chaintest.c, documents them; both of those headers pull
 * this one in with a single #include, so every existing caller keeps
 * working unchanged (the same reasoning meter.h gives for capture.h,
 * P9.3).
 */
#ifndef ACQUISITION_H
#define ACQUISITION_H

#include <stdint.h>
#include <stdbool.h>

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
 * changed, core on, DMA set up from scratch on the next start. */
uint32_t capture_set_pll(uint32_t p1, uint32_t p2);
/* The same switch, but addressed by a rate instead of by divider
 * settings: the closest combination of PLL1 output dividers and PLLFBDIV
 * is chosen (clock.h), so the caller says 8000 and gets 8000. *got_ksps
 * is what the hardware will deliver - always read it, the wish is not
 * always reachable exactly. */
uint32_t capture_set_rate(uint32_t want_ksps, uint32_t *got_ksps);
uint32_t capture_set_clkdiv(uint32_t ratio_h);

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
 * capture_oneshot() (meter.h) then runs it. False if the variant could
 * not be set up at all (a clock that did not come, a period out of
 * range).
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

/* ---- The standing stream ("stream on"/"stream grab", chaintest.c until
 * P9.4) ----
 *
 * The chain as a standing stream, the way the example runs it:
 * chain_stream_on() sets it up at about `ksps` (the nearest 160 MHz / N,
 * 1..40000) with the DAC triangle on RA8 as the signal and returns; main()
 * then processes every half. chain_stream_off() stops it and restores the
 * boot configuration; chain_all(), chain_run() (chaintest.h) and the old
 * tests call it first. chain_stream_state(): rate, transfers since the
 * start, free CPU cycles per sample; false if no stream is on, or if it
 * stopped itself (overrun brake) - chain_streaming() still says one was
 * started. */
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

#endif /* ACQUISITION_H */

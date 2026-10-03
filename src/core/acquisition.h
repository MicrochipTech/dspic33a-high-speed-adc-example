/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

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
 * P9.4b (27.09.2026) added the chain setup itself: acq_chain_setup()/
 * acq_chain_restore()/acq_triangle_for()/acq_rate_hz()/acq_ksps_of()/
 * acq_wait_ticks(), moved out of chaintest.c (there as setup()/restore()/
 * triangle_for()/rate_hz()/ksps_of()/wait_ticks()). P9.4 had left them in
 * chaintest.c and reached them from chain_stream_on_input() (above) through
 * a chaintest_priv.h - the application layer depending on the test layer's
 * internals, the wrong direction, through generic global names besides.
 * chaintest.c's own stages ("chain all", "chain run") call these same six
 * functions now declared here, the normal test -> app direction; the raw
 * state a few of them still share with chaintest.c (the measured trigger
 * frequency, the setup diagnostics, the CPU-stepped ADC flag) is declared
 * in acquisition_priv.h instead - too raw for this public header. P11.3's
 * routing_apply() (routing.c, 27.09.2026) calls acq_chain_setup_input()
 * (below - acq_chain_setup() with the route's input) and acq_chain_
 * restore(), which is why they belong here rather than staying
 * chaintest.c's private business; ROUTE_STREAM/ROUTE_B2B (routing.h) are
 * defined in acquisition.c from the same board.h macros this code runs.
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

/* ---- The chain setup itself (chaintest.c until P9.4b) ----
 *
 * What chain_stream_on_input() (below) and chaintest.c's "chain all"/
 * "chain run" both need to bring SCCP1 -> ADC core 5 -> DMA0 up (or back
 * down) and to convert between an SCCP1 period in ticks and a rate in Hz
 * or kSPS. Moved out of chaintest.c on 27.09.2026 (P9.4b,
 * docs/IMPLEMENTATION-PLAN.md), bodies unchanged apart from the rename -
 * chaintest.c called them as setup()/restore()/triangle_for()/rate_hz()/
 * ksps_of()/wait_ticks(), generic names in the whole firmware's namespace,
 * reached from here through a chaintest_priv.h that made the application
 * layer depend on the test layer's internals. The input a caller wants
 * (core, PINSEL, SAMC, whether to drive the DAC2 triangle) and acq_chain_
 * setup()'s own report stay as static/extern state in acquisition.c/
 * acquisition_priv.h - see the "call it, do not read its internals" note
 * there. */

/* The clock tree and core switch: DMA down, SCCP1 stopped, half length at
 * the maximum, PLL1 at 320 MHz, CLKGEN13 on, the DAC's own clock selected,
 * DAC2 started at mid-scale if the caller wants the test signal, the input
 * selected, ADC in Single mode on the SCCP1 trigger. acq_chain_restore()
 * puts the boot configuration (board_cfg's PLL dividers, core burst mode)
 * back and clears the counters. */
bool acq_chain_setup(void);
void acq_chain_restore(void);

/* acq_chain_setup() with the input chosen for this one call - ADC core
 * 1..5, PINSEL, SAMC, and whether DAC2 is started as the signal - and the
 * chain-test default (core 5, RA8, SAMC 0, with the DAC) put back
 * afterwards for chaintest.c's own acq_chain_setup() calls. What routing_
 * apply() (routing.h, P11.3) calls - and since P11.4 (27.09.2026) its only
 * caller: chain_stream_on()/_on_input() go through routing_apply(). */
bool acq_chain_setup_input(uint8_t core, uint8_t pinsel, uint8_t samc, bool test_dac);

/* Pick SLPDAT so that one triangle slope lasts about SLOPE_TARGET samples
 * at `rate` Hz, the widest range the DAC's limits allow. */
bool acq_triangle_for(uint32_t rate, uint16_t *slp_out);

/* acq_trig_hz arithmetic (acquisition_priv.h): the sample rate an SCCP1
 * period of n ticks gives, and the period n ticks corresponds to. */
uint32_t acq_rate_hz(uint32_t n);
uint32_t acq_ksps_of(uint32_t n);

/* Busy-wait t Timer1 ticks. */
void acq_wait_ticks(uint32_t t);

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
 * PINSEL 3 (RA8), SAMC 0, with the triangle. Since P11.4 (27.09.2026) both
 * go through routing_apply() (routing.h): chain_stream_on() applies
 * ROUTE_STREAM, this one a route built from its arguments (ROUTE_SRC_DAC_PIN/
 * dac 2 with test_signal, ROUTE_SRC_EXT without), so a PINSEL the core does
 * not reach (route_pin_reachable(): not a package pin of that core on this
 * device, and not one of the internal channels 6/7, or 5/8 on core 5) is
 * refused - the one refusal the routing added to what this firmware accepted
 * before. Returns false as for any other set-up failure. */
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
    uint32_t proc;      /* the filter win went through: 0 none, 1 lp, 2 hp,
                         * 3 bp (sigproc.h; 0/1 only until 02.10.2026)     */
    uint32_t load_pm;   /* the processing's mean share of a half period since
                         * the previous grab, per mille (1000 = it just keeps
                         * up) - capture_service()'s own proc_ticks_* deltas */
} chain_grab_t;

bool chain_stream_grab_begin(chain_grab_t *g);
bool chain_stream_grab_end(void);

#endif /* ACQUISITION_H */

/*
 * acquisition.c
 *
 * Choosing and running the acquisition, moved out of capture.c and
 * chaintest.c on 27.09.2026 (P9.4, docs/IMPLEMENTATION-PLAN.md):
 *
 *   - capture.c's rate setters (capture_set_pll(), capture_set_rate(),
 *     capture_set_clkdiv()) and the variant matrix (capture_select_
 *     variant() and its reporting functions) - bodies unchanged;
 *   - chaintest.c's standing stream (chain_stream_on()/_on_input()/_off()/
 *     _streaming()/_state()/_grab_begin()/_grab_end() and their private
 *     state, the s_on/s_ticks/s_slpdat trio and the grab baseline) -
 *     bodies unchanged. chain_streaming() and chain_stream_state() were
 *     not named in the card that started this move, but both only read
 *     s_on/s_ticks - moving them here too avoided a second, reverse-
 *     direction private header just for two one-line readers.
 *
 * P9.4b (27.09.2026, docs/IMPLEMENTATION-PLAN.md) added what chain_stream_
 * on_input() (below) needs to bring the chain up in the first place:
 * chaintest.c's setup()/restore()/triangle_for()/rate_hz()/ksps_of()/
 * wait_ticks(), renamed acq_chain_setup()/acq_chain_restore()/acq_
 * triangle_for()/acq_rate_hz()/acq_ksps_of()/acq_wait_ticks(), bodies
 * unchanged apart from the rename. P9.4 had left them in chaintest.c and
 * reached them from here through a chaintest_priv.h - the application
 * layer (this file) depending on the test layer's (chaintest.c's)
 * internals, backwards, and through non-static globals with generic names
 * in the whole firmware's namespace besides. Now chaintest.c's own stages
 * ("chain all", "chain run") call these six functions here instead - test
 * depending on app, the normal direction (CLAUDE.md's module table).
 * s_core/s_pinsel/s_samc/s_test_dac (the input acq_chain_setup() applies)
 * moved with them and went back to being a plain static below: their only
 * reader was setup() itself, their only writer already chain_stream_
 * on_input(), so both ends of that state are in this one file now.
 *
 * Private state these functions need stayed where it was, or moved here
 * with them, reached through two narrow, non-public headers - the same
 * pattern P9.3 set for meter.c:
 *
 *   capture_priv.h        clkdiv_cur (capture.c): capture_set_clkdiv() is
 *                          the only writer, capture_clkdiv_wanted() (still
 *                          in capture.c) the only other reader.
 *   acquisition_priv.h    acq_trig_hz, acq_setup_rc_pll/_trig/_dac/_ok and
 *                          acq_step_on (this file): also written or read
 *                          directly by "chain all"'s stages and "chain
 *                          run" (chaintest.c), which is why they are not
 *                          plain statics here - see that header's comment
 *                          for each one. Replaces chaintest.c's P9.4
 *                          chaintest_priv.h, deleted by P9.4b.
 */

/* <xc.h> is not used directly here (no register access - everything goes
 * through adc.c/clock.c/sccp.c/capture.c's own functions), but adc.h
 * pulls in board.h, whose BOARD_EV74H48A branch checks
 * __dsPIC33AK512MPS512__ - a real-hardware build gets that as a compiler
 * builtin from -mcpu regardless, but the host trace harness's fake device
 * header only defines it where <xc.h> is included, exactly like
 * capture.c/chaintest.c/meter.c already do for the same reason. */
#include <xc.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "acquisition.h"
#include "acquisition_priv.h"
#include "routing.h"         /* route_t: ROUTE_STREAM/ROUTE_B2B are defined here (P11.3) */
#include "capture.h"
#include "capture_priv.h"
#include "adc.h"
#include "clock.h"
#include "sccp.h"
#include "dac.h"
#include "timebase.h"
#include "console.h"
#include "diag.h"

/* ------------------------------------------------------------------ *
 * The ADC clock - the rate setters (capture.c until P9.4, 27.09.2026)
 * ------------------------------------------------------------------ */
uint32_t capture_set_clkdiv(uint32_t ratio_h)
{
    /* Kept for the record and for "clk": the CLKGEN6 divider does arrive
     * in the register but does not change the conversion rate on this
     * silicon (runs 8 and 9). The rate is set with capture_set_pll(). */
    const bool restart = capture_settle();      /* DMA down, burst ended */
    adc_deinit();                               /* core off              */
    const uint32_t rc    = clock_adc_set_div(ratio_h);
    const bool     ready = adc_reinit();        /* core on, ADRDY        */
    if (rc == CLKDIV_OK) { clkdiv_cur = ratio_h; }
    if (restart) { capture_start(); }           /* dma0_init() again     */
    if (rc != CLKDIV_OK) { return rc; }
    return ready ? CLKDIV_OK : CLKDIV_ADC;
}

uint32_t capture_set_pll(uint32_t p1, uint32_t p2)
{
    /* Same order as the divider and as the boot: DMA channel down and
     * burst finished, ADC core off (the PLL's output dividers must not
     * move while something runs off them, p778), PLL retuned, core on
     * with ADRDY, DMA set up from scratch on the next start. */
    const bool restart = capture_settle();
    adc_deinit();
    const uint32_t rc    = clock_adc_set_pll(p1, p2);
    const bool     ready = adc_reinit();
    if (restart) { capture_start(); }
    if (rc != CLKDIV_OK) { return rc; }
    return ready ? CLKDIV_OK : CLKDIV_ADC;
}

uint32_t capture_set_rate(uint32_t want_ksps, uint32_t *got_ksps)
{
    /* Same order as every other clock change here, and the same reason:
     * the DMA channel and the ADC core are what run off this clock, so
     * they go down first and come back the way the boot brings them up. */
    const bool restart = capture_settle();
    adc_deinit();
    const uint32_t rc    = clock_adc_set_rate(want_ksps, got_ksps);
    const bool     ready = adc_reinit();
    if (restart) { capture_start(); }
    if (rc != CLKDIV_OK) { return rc; }
    return ready ? CLKDIV_OK : CLKDIV_ADC;
}

/* ------------------------------------------------------------------ *
 * The variant matrix (capture.c until P9.4, 27.09.2026)
 * ------------------------------------------------------------------ */
static capture_variant_t var_cur      = CAP_VAR_B2B;
static uint32_t          var_ksps     = 0u;   /* what it should deliver */
static uint32_t          var_trig_ns  = 0u;   /* 0 = untriggered        */

const char *capture_variant_name(capture_variant_t v)
{
    switch (v) {
    case CAP_VAR_B2B:         return "back-to-back, rate from PLL1";
    case CAP_VAR_SCCP_T_PER:  return "SCCP1 timer + special event, peripheral clock";
    case CAP_VAR_SCCP_T_G13:  return "SCCP1 timer + special event, CLKGEN13";
    case CAP_VAR_SCCP_OC_PER: return "SCCP1 output compare, peripheral clock";
    case CAP_VAR_SCCP_OC_G13: return "SCCP1 output compare, CLKGEN13";
    case CAP_VAR_SCCP_OLD:    return "SCCP1 as in runs 5-7 (trigger 34, rollover)";
    case CAP_VAR_SCCP_TRG2:   return "SCCP1 special event as TRG2 inside a burst";
    case CAP_VAR_RPTCNT:      return "ADC repeat timer (RPTCNT)";
    case CAP_VAR_OVERSAMPLE:  return "oversampling, ACCNUM divides the event rate";
    case CAP_VAR_CLKDIV:      return "CLKGEN6 divider";
    default:                  return "?";
    }
}

uint32_t capture_variant_ksps(void)      { return var_ksps; }
uint32_t capture_trigger_period_ns(void) { return var_trig_ns; }

/* The PLL pair whose rate is closest to the wish, from the sweep ladder. */
static struct pll_step pll_for(uint32_t want_ksps, uint32_t *got_ksps)
{
    uint32_t n = 0u;
    const struct pll_step *st = capture_sweep_steps(&n);
    struct pll_step best = st[0];
    uint32_t best_d = 0xFFFFFFFFu, best_k = 0u;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t k = (uint32_t)(1600000u / ((uint32_t)st[i].p1 * st[i].p2) / 8u);
        const uint32_t d = (k > want_ksps) ? (k - want_ksps) : (want_ksps - k);
        if (d < best_d) { best_d = d; best = st[i]; best_k = k; }
    }
    if (got_ksps != NULL) { *got_ksps = best_k; }
    return best;
}

bool capture_select_variant(capture_variant_t v, uint32_t want_ksps)
{
    if ((v >= CAP_VAR_COUNT) || (want_ksps == 0u)) { return false; }

    (void)capture_settle();           /* idle before anything changes   */
    sccp1_stop();
    var_cur     = v;
    var_ksps    = 0u;
    var_trig_ns = 0u;

    /* Every variant starts from the same base: the ADC clock undivided
     * and at full speed, so that only the variant's own mechanism can
     * account for a rate below it. The PLL variant overrides this. */
    if (v != CAP_VAR_B2B) {
        adc_deinit();
        (void)clock_adc_set_div(100u);
        (void)clock_adc_set_pll(5u, 1u);   /* 320 MHz                   */
        (void)adc_reinit();
    }

    switch (v) {
    case CAP_VAR_B2B: {
        uint32_t got = 0u;
        const struct pll_step st = pll_for(want_ksps, &got);
        adc_deinit();
        if (clock_adc_set_pll(st.p1, st.p2) != CLKDIV_OK) { (void)adc_reinit(); return false; }
        if (!adc_reinit()) { return false; }
        adc_set_mode_burst();
        var_ksps = got;
        break;
    }

    case CAP_VAR_SCCP_T_PER:
    case CAP_VAR_SCCP_T_G13:
    case CAP_VAR_SCCP_OC_PER:
    case CAP_VAR_SCCP_OC_G13:
    case CAP_VAR_SCCP_OLD:
    case CAP_VAR_SCCP_TRG2: {
        const bool        g13  = (v == CAP_VAR_SCCP_T_G13) || (v == CAP_VAR_SCCP_OC_G13) ||
                                 (v == CAP_VAR_SCCP_TRG2);
        const bool        oc   = (v == CAP_VAR_SCCP_OC_PER) || (v == CAP_VAR_SCCP_OC_G13);
        const bool        old  = (v == CAP_VAR_SCCP_OLD);
        const sccp_clk_t  clk  = g13 ? SCCP_CLK_GEN13 : SCCP_CLK_PERIPHERAL;
        const sccp_mode_t mode = oc  ? SCCP_MODE_OC   : SCCP_MODE_TIMER;
        const sccp_event_t ev  = old ? SCCP_EVENT_ROLLOVER : SCCP_EVENT_SPECIAL;
        const uint8_t     trg  = old ? SCCP3_ADC_TRIGGER : SCCP1_ADC_TRIGGER;

        if (g13 && !clock_trig_on()) { return false; }
        /* ticks of the module's own clock per sample */
        const uint32_t hz = g13 ? clock_trig_hz() : clock_periph_hz();
        const uint32_t ticks = hz / 1000u / want_ksps;
        if (ticks < 2u) { return false; }
        if (!sccp1_start(ticks, clk, mode, ev)) { return false; }

        if (v == CAP_VAR_SCCP_TRG2) {
            adc_set_mode_burst();     /* software starts, SCCP continues */
            adc_set_trg2(trg);
        } else {
            adc_set_mode_single(trg); /* one conversion per trigger      */
        }
        var_ksps    = sccp1_nominal_ksps();
        var_trig_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / hz);
        break;
    }

    case CAP_VAR_RPTCNT: {
        /* TAD is a quarter of the ADC clock and a conversion is two TAD,
         * so the repeat timer's period in TAD gives 80000/n kSPS at
         * 320 MHz. 2..63. */
        uint32_t n = 80000u / want_ksps;
        if (n < 2u)  { n = 2u; }
        if (n > 63u) { n = 63u; }
        adc_set_mode_burst();
        adc_set_period((uint8_t)n);
        adc_set_trg2(0x03u);          /* repeat timer                    */
        var_ksps = 80000u / n;
        break;
    }

    case CAP_VAR_OVERSAMPLE: {
        /* ACCNUM is two bits. The event rate should be the conversion
         * rate divided by the accumulation count. */
        uint32_t acc = 0u;
        const uint32_t ratio = 40000u / want_ksps;
        if      (ratio >= 8u) { acc = 3u; }
        else if (ratio >= 4u) { acc = 2u; }
        else if (ratio >= 2u) { acc = 1u; }
        adc_set_mode_oversample((uint8_t)acc);
        var_ksps = 40000u >> acc;
        break;
    }

    case CAP_VAR_CLKDIV: {
        uint32_t ratio = 4000000u / want_ksps;
        if (ratio < 100u)  { ratio = 100u; }
        if (ratio > 1000u) { ratio = 1000u; }
        if ((ratio > 100u) && (ratio < 200u)) { ratio = 200u; }
        adc_deinit();
        const uint32_t rc = clock_adc_set_div(ratio);
        (void)adc_reinit();
        if (rc != CLKDIV_OK) { return false; }
        adc_set_mode_burst();
        var_ksps = 4000000u / ratio;
        break;
    }

    default:
        return false;
    }
    return true;
}

void capture_variant_regs(void)
{
    console_kv("[var]   adc MODE", adc_mode());
    console_kv("[var]   adc TRG1SRC", adc_trg1());
    console_kv("[var]   adc TRG2SRC", adc_trg2());
    console_kv("[var]   adc ACCNUM", adc_accnum());
    console_kv("[var]   adc RPTCNT", adc_period());
    console_kv("[var]   adc clock Hz", clock_adc_hz());
    console_kv("[var]   clkgen6 ratio x100", clock_adc_div());
    if (var_trig_ns != 0u) {
        console_kv("[var]   trigger period ns", var_trig_ns);
        sccp1_regs_visit(reg_print);
    }
}

/* ------------------------------------------------------------------ *
 * The chain setup itself (chaintest.c until P9.4b, 27.09.2026) - shared by
 * chain_stream_on_input() (below) and chaintest.c's "chain all"/"chain run"
 * ------------------------------------------------------------------ */
uint32_t     acq_trig_hz = TRIG_HZ_NOMINAL;   /* measured in chaintest.c's S1 */
uint32_t     acq_setup_rc_pll;
bool         acq_setup_trig, acq_setup_dac, acq_setup_ok;
volatile bool acq_step_on = false;            /* chaintest.c's CPU-stepped ADC flag */

uint32_t acq_rate_hz(uint32_t n)       { return acq_trig_hz / n; }
uint32_t acq_ksps_of(uint32_t n)       { return (acq_trig_hz / 1000u + n / 2u) / n; }

void acq_wait_ticks(uint32_t t)
{
    const uint32_t t0 = timebase_ticks();
    while ((timebase_ticks() - t0) < t) { }
}

/* setup() with the input the chain samples: core, PINSEL, SAMC, and
 * whether this module drives DAC2 as the test signal (the chain test and
 * "stream on <ksps>" do; "stream on <ksps> <core> <pinsel>" leaves the
 * DAC to whoever set it up - the GUI's DAC controls). Both ends of this
 * state are in this file since P9.4b, so it is a plain static again. */
static uint8_t s_core = CHAIN_CORE, s_pinsel = CHAIN_PINSEL, s_samc = CHAIN_SAMC;
static bool    s_test_dac = true;

/* The two paths as route_t data (routing.h, P11.3, 27.09.2026), defined
 * here so that they are built from the same board.h macros the code below
 * runs them with - CHAIN_CORE/CHAIN_PINSEL/CHAIN_SAMC (chain_stream_on(),
 * acq_chain_setup()'s defaults above) and ADC_INSTANCE/ADC_PINSEL/ADC_SAMC
 * (acq_chain_restore()) - on both boards, without routing.h including
 * board.h. ROUTE_STREAM's DAC2 rides its own pin (DACOUT2 = RA8 = AD5AN3,
 * board.h), so ROUTE_SRC_DAC_PIN, dac 2; ROUTE_B2B has no signal of its
 * own. ROUTE_B2B is data only in N+1: nothing applies it yet. */
const route_t ROUTE_STREAM = {
    .src = ROUTE_SRC_DAC_PIN, .core = CHAIN_CORE, .pinsel = CHAIN_PINSEL,
    .dac = 2u, .sink = ROUTE_SINK_STREAM, .table_samples = 0u,
    .samc = CHAIN_SAMC,
};
const route_t ROUTE_B2B = {
    .src = ROUTE_SRC_EXT, .core = ADC_INSTANCE, .pinsel = ADC_PINSEL,
    .dac = 0u, .sink = ROUTE_SINK_STREAM, .table_samples = 0u,
    .samc = ADC_SAMC,
};

bool acq_chain_setup(void)
{
    timebase_init();
    (void)capture_settle();
    sccp1_stop();
    (void)capture_set_half_len(SAMPLES_PER_HALF_MAX);
    acq_setup_rc_pll = capture_set_pll(5u, 1u);       /* 320 MHz, VCO 1600 */
    acq_setup_trig   = clock_trig_on();               /* CLKGEN13 160 MHz  */
    clock_dac_select(CLOCK_DAC_PLL1_VCO);
    acq_setup_dac    = s_test_dac ? dac2_level_start(0x800u) : true;   /* CLKGEN7 400 MHz */
    (void)capture_select_core(s_core, s_pinsel, s_samc);
    adc_set_mode_single(SCCP1_ADC_TRIGGER);
    adc_set_irqsel(0u);
    (void)capture_settle();                           /* DMA down          */
    if (acq_trig_hz == 0u) { acq_trig_hz = TRIG_HZ_NOMINAL; }
    acq_setup_ok = (acq_setup_rc_pll == CLKDIV_OK) && acq_setup_trig && acq_setup_dac;
    return acq_setup_ok;
}

/* acq_chain_setup() with the input chosen for this one call: until P11.4
 * (27.09.2026) the three lines chain_stream_on_input() wrapped around its
 * own acq_chain_setup() call, as a function, so that routing_apply()
 * (routing.c, P11.3) can run the same setup for a route_t without reaching
 * this file's statics. Since P11.4 routing_apply() is the ONLY caller:
 * chain_stream_on()/_on_input() (below) go through it, the inline copy is
 * gone, and this is the one place the chain's input is set. */
bool acq_chain_setup_input(uint8_t core, uint8_t pinsel, uint8_t samc, bool test_dac)
{
    s_core = core; s_pinsel = pinsel; s_samc = samc; s_test_dac = test_dac;
    const bool ok = acq_chain_setup();
    s_core = CHAIN_CORE; s_pinsel = CHAIN_PINSEL; s_samc = CHAIN_SAMC; s_test_dac = true;
    return ok;
}

void acq_chain_restore(void)
{
    (void)capture_settle();
    sccp1_stop();
    sccp1_count(false);
    adc_ch0_irq(false, false);
    acq_step_on = false;
    dac2_off();
    clock_dac_select(CLOCK_DAC_PLL1_VCO);
    (void)capture_select_core(ADC_INSTANCE, ADC_PINSEL, ADC_SAMC);  /* burst mode again */
    (void)capture_set_pll(board_cfg.adc_pll_postdiv1, board_cfg.adc_pll_postdiv2);
    counters_clear();
}

/* Pick SLPDAT so that one slope lasts about SLOPE_TARGET samples at this
 * rate, with the widest range the limits allow: DACLOW >= 0xCD + SLPDAT
 * and DACDAT <= 0xF32 - SLPDAT (note 1 of Example 18-3, p1422), 32 codes
 * of margin inside that. The slope in samples is
 *   span * 32 * rate / (SLPDAT * F_DAC),   span = 0xE65 - 2 * SLPDAT - 64,
 * which falls as SLPDAT rises, so the first SLPDAT at or below the target
 * is taken. 128 samples: short enough for about 16 turning points per
 * window, so that a fault almost anywhere in it is enclosed by four of
 * them (see tri_eval), and steep enough (about 27 LSB per sample) for a
 * turning point to a few hundredths of a sample; long enough that the
 * corners the DAC's output filter rounds (600 ns, Table 40-43) stay in
 * the eighth of each slope the fit leaves out, even at 40 MSPS. At
 * 100 kSPS the slowest triangle the DAC makes lasts only about 29
 * samples per slope; that is what it gets there. */
#define SLOPE_TARGET  128u
bool acq_triangle_for(uint32_t rate, uint16_t *slp_out)
{
    const uint32_t f = clock_dac_hz();
    if (f == 0u) { return false; }
    const uint32_t full = DAC_CODE_MAX - DAC_CODE_MIN - 64u;       /* 3621 */
    uint32_t s = 1u;
    for (; (2u * s + 64u) < full; s++) {
        const uint64_t span = full - 2u * s;
        const uint64_t samples = span * 32u * rate / ((uint64_t)s * f);
        if (samples <= SLOPE_TARGET) { break; }
    }
    *slp_out = (uint16_t)s;
    return dac2_triangle_start((uint16_t)(DAC_CODE_MIN + s + 32u),
                               (uint16_t)(DAC_CODE_MAX - s - 32u), (uint16_t)s);
}

/* ------------------------------------------------------------------ *
 * The chain as a standing stream ("stream on") - chaintest.c until P9.4
 * (27.09.2026) - the example itself
 *
 * Set up as the test does, the DAC triangle on RA8 as the signal, the
 * triggered stream started with no end, and then the command RETURNS:
 * from here on main() serves every completed half through
 * capture_service(), exactly as the application will, and the console
 * stays free. Nothing prints by itself while it runs; "stream" asks.
 * ------------------------------------------------------------------ */
static bool     s_on     = false;
static uint32_t s_ticks  = 0u;
static uint16_t s_slpdat = 0u;

/* Baselines for chain_stream_grab_begin()'s per-cycle counters: the
 * lifetime counter as it stood after the previous grab (or after
 * "stream on", for the first one - all of them are 0 then, because
 * capture_chain_start() calls counters_clear()). */
static uint32_t g_grab_ov0, g_grab_la0, g_grab_mi0, g_grab_hv0;
static uint64_t g_grab_xf0;

static uint32_t period_for(uint32_t ksps)
{
    uint32_t n = (acq_trig_hz / 1000u + ksps / 2u) / ksps;
    return (n < 4u) ? 4u : n;                 /* 40 MSPS is the ceiling      */
}

/* The one setup path of "stream on" since P11.4 (27.09.2026): the route is
 * brought up by routing_apply() (routing.c) - route_check() first (no
 * register: pin reachability, core exclusivity, the resource table, the
 * sink gate), then acq_chain_setup_input(core, pinsel, samc, test_dac) with
 * the route's input and DAC choice, acq_chain_restore() if the clock tree,
 * trigger clock or DAC refused - the very statements this function ran
 * inline before, so the register sequence is the one the `stream_on`/
 * `stream_on_input` goldens fix (tests/trace). routing_apply() leaves the
 * trigger stopped and the DMA channel down; the rate is not a route's
 * (routing.h, requirement A1), so the tail here starts it, as before.
 *
 * Whether DAC2 is started as the signal is the route's `src`: ROUTE_SRC_
 * DAC_PIN (ROUTE_STREAM, chain_stream_on()) starts it, ROUTE_SRC_EXT (the
 * custom input) leaves it alone - exactly the old test_signal flag.
 *
 * The route is recorded on success and released by routing_clear() when
 * the stream is torn down - chain_stream_off(), and the one failure path
 * after a successful apply - so that the next "stream on" is not refused
 * as a core conflict with the previous one. */
static bool stream_on_route(uint32_t ksps, const route_t *r)
{
    chain_stream_off();
    if (CHAIN_ON_SIMULATOR || (ksps == 0u) || (r->core < 1u) || (r->core > 5u) ||
        (r->pinsel > 15u) || (r->samc > 31u)) { return false; }
    acq_trig_hz = TRIG_HZ_NOMINAL;
    if (routing_apply(r) != ROUTE_OK) { return false; }   /* restored by routing_apply() */
    const uint32_t n = period_for(ksps);
    uint16_t slp = 0u;                        /* 0 in the frame: no test triangle */
    if (r->src == ROUTE_SRC_DAC_PIN) { (void)acq_triangle_for(acq_rate_hz(n), &slp); }
    acq_wait_ticks(TICKS_PER_MS);
    if (!capture_chain_start(n, SCCP_MODE_TIMER, 0u, false)) {
        acq_chain_restore();
        routing_clear();
        return false;
    }
    s_on     = true;
    s_ticks  = n;
    s_slpdat = slp;
    g_grab_ov0 = 0u; g_grab_la0 = 0u; g_grab_mi0 = 0u; g_grab_hv0 = 0u; g_grab_xf0 = 0u;
    return true;
}

bool chain_stream_on(uint32_t ksps)
{
    return stream_on_route(ksps, &ROUTE_STREAM);
}

bool chain_stream_on_input(uint32_t ksps, uint8_t core, uint8_t pinsel, uint8_t samc,
                           bool test_signal)
{
    const route_t r = {
        .src = test_signal ? ROUTE_SRC_DAC_PIN : ROUTE_SRC_EXT,
        .core = core, .pinsel = pinsel, .dac = test_signal ? 2u : 0u,
        .sink = ROUTE_SINK_STREAM, .table_samples = 0u, .samc = samc,
    };
    return stream_on_route(ksps, &r);
}

void chain_stream_off(void)
{
    if (!s_on) { return; }
    (void)capture_chain_stop();
    acq_chain_restore();
    routing_clear();
    s_on = false;
}

bool chain_streaming(void)
{
    return s_on;
}

bool chain_stream_grab_begin(chain_grab_t *g)
{
    if (!s_on) { return false; }
    if (!capture_chain_halt()) {
        /* The brake fired, or the chain was already down under us: leave
         * nothing half-configured, and let "stream" say it is off. */
        chain_stream_off();
        return false;
    }
    g->win     = capture_completed_half();
    g->win_len = capture_half_len();
    g->from    = (uint32_t)(g->win - capture_buffer());
    g->ksps    = acq_ksps_of(s_ticks);
    const uint32_t ov = dma_overrun, la = late_service, mi = proc_missed, hv = blocks_done;
    const uint64_t xf = capture_transfers();
    g->overrun   = ov - g_grab_ov0;
    g->late      = la - g_grab_la0;
    g->missed    = mi - g_grab_mi0;
    g->halves    = hv - g_grab_hv0;
    g->transfers = (uint32_t)(xf - g_grab_xf0);
    g_grab_ov0 = ov; g_grab_la0 = la; g_grab_mi0 = mi; g_grab_hv0 = hv; g_grab_xf0 = xf;
    g->slpdat  = s_slpdat;
    g->dac_hz  = clock_dac_hz();
    return true;
}

bool chain_stream_grab_end(void)
{
    if (!s_on) { return false; }
    if (!capture_chain_resume()) {
        chain_stream_off();
        return false;
    }
    return true;
}

bool chain_stream_state(uint32_t *ksps, uint64_t *transfers, uint32_t *free_cyc)
{
    if (!s_on) { return false; }
    *ksps = acq_ksps_of(s_ticks);
    *transfers = capture_transfers();
    /* The budget as in S6: a half lasts half_len * N / f_trig, 16 CPU
     * cycles per Timer1 tick, minus the mean processing time. */
    const uint32_t hl = capture_half_len();
    const uint32_t half_ticks = (uint32_t)(((uint64_t)hl * s_ticks * TIMEBASE_HZ) / acq_trig_hz);
    const uint32_t pmean = (proc_count != 0u) ? (proc_ticks_sum / proc_count) : 0u;
    *free_cyc = (half_ticks > pmean) ? ((half_ticks - pmean) * CPU_PER_TICK / hl) : 0u;
    return capture_chain_active();            /* false once the brake fired  */
}

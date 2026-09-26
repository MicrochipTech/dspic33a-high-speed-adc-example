/*
 * variants.c (scenario, P0.5) - capture_select_variant() for every entry
 * of capture_variant_t (capture.h), capture.c compiled unchanged, all at
 * the same want_ksps = 100 so that the ten calls differ only in the
 * variant's own mechanism, not in whether the target rate was reachable
 * at all.
 *
 * Why 100, not a rounder "customer" number like 8000: the two SCCP-clocked
 * paths (CAP_VAR_SCCP_T_PER/OC_PER/OLD on the peripheral clock,
 * CAP_VAR_SCCP_T_G13/OC_G13/TRG2 on CLKGEN13) compute
 * `ticks = hz / 1000 / want_ksps` and refuse (sccp1_start() returns false)
 * if that rounds to below 2 - found the hard way, first with want_ksps =
 * 8000: this scenario never calls clock_init() (that is `boot`'s job), so
 * the peripheral clock is still the 8 MHz FRC / 2 = 4 MHz clock_periph_hz()
 * reads at reset, and 4 000 000 / 1000 / 8000 = 0. 100 keeps every variant
 * comfortably above that floor without needing to re-run clock_init() here.
 * PLL1DIV is still preset to its clock_init() boot value (PLLFBDIV/PLLPRE,
 * as the `dac`/`stream_on` scenarios also do) so CAP_VAR_SCCP_T_G13/
 * OC_G13/TRG2's CLKGEN13-derived clock is a real number and not the "PLL1
 * never configured" 1.6 MHz a fully-reset PLL1DIV would give.
 *
 * capture_select_variant() settles first (capture_settle(), a no-op here:
 * nothing has started a burst) and, for every variant but CAP_VAR_B2B,
 * resets the ADC clock to undivided/320 MHz before applying the variant -
 * so PLL1CON/OSCCTRL/CLK6CON's hardware-model rules are needed throughout,
 * and CLK13CON's for the two "G13" variants and CAP_VAR_SCCP_TRG2 (see
 * the `sccp` scenario's identical rule and comment). AD3CON.ADRDY: the
 * board's default core (ADC_INSTANCE 3) - this scenario never switches
 * core.
 */
#include <setjmp.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "capture.h"

extern jmp_buf fail_jmp;

static const hwmodel_rule_t rules[] = {
    { "PLL1CON",  _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL",  0u, _OSCCTRL_PLL1RDY_MASK },
    { "CLK6CON",  _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "CLK13CON", _CLK13CON_OSWEN_MASK | _CLK13CON_DIVSWEN_MASK, _CLK13CON_CLKRDY_MASK },
    { "AD3CON",   0u, _AD3CON_ADRDY_MASK },
};

/* Called three times, UNCONDITIONALLY - not "until it returns true" like
 * every other scenario's retry (dac.c, sccp.c, b2b.c, clk.c,
 * stream_on(_input).c). capture_select_variant()'s own return value is
 * not a reliable signal of whether its PREAMBLE's clock switch raced: for
 * every variant but CAP_VAR_B2B it calls
 * `(void)clock_adc_set_div(100); (void)clock_adc_set_pll(5u, 1u);` -
 * both return codes explicitly discarded (capture.c) - before the
 * per-variant switch, so a preamble wait that the hardware model's
 * residual scheduling race (tests/trace/README.md, "hardware model - a
 * residual race") answered too late can leave PLL1CON/CLK6CON with a
 * self-clearing bit still set, WHILE the function goes on to return true
 * (the SCCP/RPTCNT/... part after the preamble succeeded on its own).
 * Found exactly this way: `CAP_VAR_SCCP_OC_PER` once returned 1 with
 * `PLL1CON 0x0 -> 0x10000000` (FOUTSWEN stuck) and `CLK6CON 0x80000000
 * -> 0x80400000` (DIVSWEN stuck) sitting in the trace, undetected by a
 * return-value check because there was nothing to retry against - the
 * call had already "succeeded". Calling capture_select_variant() again
 * redoes the SAME preamble from scratch, which either finds the model has
 * since cleared the stale bit (idempotent - no new diff) or gives it a
 * fresh, fully-warmed window to do so; repeated three times the residual
 * per-attempt race chance (tests/trace/README.md) compounds to a
 * practically negligible one. In the ordinary (non-racy) case the second
 * and third calls rewrite every register to the value it already holds,
 * which a snapshot diff shows as no change at all - so this changes
 * nothing about a clean run's trace, only closes the gap this one has. */
static void try_variant(capture_variant_t v, const char *label)
{
    if (setjmp(fail_jmp) == 0) {
        bool ok = false;
        for (int attempt = 0; attempt < 3; attempt++) {
            ok = capture_select_variant(v, 100u);
        }
        trace_note("# capture_select_variant(%s, 100) -> %d\n", label, (int)ok);
    } else {
        trace_note("# capture_select_variant(%s, 100) called fail() - see the F line above\n", label);
    }
    trace_point(label);
}

int main(void)
{
    trace_begin("variants");

    /* Preset: PLL1 as clock_init() leaves it at boot (see file header) -
     * only PLL1DIV, which is all clock_trig_hz() (the CLKGEN13-clocked
     * variants' rate, computed from PLL1's own output) needs. */
    PLL1DIV = 0x0100C829u;
    trace_point("preset: PLL1DIV as clock_init() leaves it at boot");

    hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);

    try_variant(CAP_VAR_B2B,         "CAP_VAR_B2B");
    try_variant(CAP_VAR_SCCP_T_PER,  "CAP_VAR_SCCP_T_PER");
    try_variant(CAP_VAR_SCCP_T_G13,  "CAP_VAR_SCCP_T_G13");
    try_variant(CAP_VAR_SCCP_OC_PER, "CAP_VAR_SCCP_OC_PER");
    try_variant(CAP_VAR_SCCP_OC_G13, "CAP_VAR_SCCP_OC_G13");
    try_variant(CAP_VAR_SCCP_OLD,    "CAP_VAR_SCCP_OLD");
    try_variant(CAP_VAR_SCCP_TRG2,   "CAP_VAR_SCCP_TRG2");
    try_variant(CAP_VAR_RPTCNT,      "CAP_VAR_RPTCNT");
    try_variant(CAP_VAR_OVERSAMPLE,  "CAP_VAR_OVERSAMPLE");
    try_variant(CAP_VAR_CLKDIV,      "CAP_VAR_CLKDIV");

    hwmodel_stop();
    trace_end();
    return 0;
}

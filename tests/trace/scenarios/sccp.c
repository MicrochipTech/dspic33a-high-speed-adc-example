/*
 * sccp.c (scenario, P0.5) - sccp1_start() for every clock/mode/event
 * combination the firmware actually uses, sccp.c and clock.c compiled
 * unchanged.
 *
 * capture_select_variant() (capture.c) drives sccp1_start() with these
 * five distinct (clk, mode, ev) tuples (capture.h's capture_variant_t):
 *
 *   CAP_VAR_SCCP_T_PER   PERIPHERAL, TIMER, SPECIAL
 *   CAP_VAR_SCCP_T_G13   GEN13,      TIMER, SPECIAL   (CAP_VAR_SCCP_TRG2
 *                                                       uses the same tuple)
 *   CAP_VAR_SCCP_OC_PER  PERIPHERAL, OC,    SPECIAL
 *   CAP_VAR_SCCP_OC_G13  GEN13,      OC,    SPECIAL
 *   CAP_VAR_SCCP_OLD     PERIPHERAL, TIMER, ROLLOVER  (the combination that
 *                                                       failed in runs 5-7)
 *
 * called here directly through sccp.h's own public sccp1_start(), rather
 * than through capture_select_variant() (which also touches the ADC and
 * the PLL - out of scope for this scenario; that path is `variants`).
 * ticks = 100 throughout: sccp1_start() only requires ticks >= 2, and the
 * exact value does not change which registers are written, only CCP1PR
 * and CCP1RB's numeric value.
 *
 * A GEN13 tuple needs clock_trig_on() (clock.c) first, exactly as
 * capture_select_variant() calls it for the g13 variants - hence
 * clock.c (and, transitively, timebase.c: clock_monitor_hz() calls
 * timebase_ticks()) is linked alongside sccp.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "clock.h"
#include "sccp.h"

static const hwmodel_rule_t rules[] = {
    /* clock_trig_on() (clock.c): CLK13CON's OSWEN/DIVSWEN self-clear,
     * CLKRDY is hardware-set - open point closed, see the `dac` scenario's
     * CLK7CON rule for the identical shape on another generator. */
    { "CLK13CON", _CLK13CON_OSWEN_MASK | _CLK13CON_DIVSWEN_MASK, _CLK13CON_CLKRDY_MASK },
};

int main(void)
{
    trace_begin("sccp");
    hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);

    bool ok;

    ok = sccp1_start(100u, SCCP_CLK_PERIPHERAL, SCCP_MODE_TIMER, SCCP_EVENT_SPECIAL);
    trace_note("# sccp1_start(PERIPHERAL, TIMER, SPECIAL) -> %d\n", (int)ok);
    trace_point("T_PER: peripheral clock, timer mode, special event trigger");

    /* Retried silently against the hardware model's residual scheduling
     * race on CLK13CON's wait - see tests/trace/README.md, "hardware
     * model - a residual race"; the `dac` scenario's identical retry has
     * the full rationale. */
    {
        bool trig_ok = false;
        for (int attempt = 0; (attempt < 5) && !trig_ok; attempt++) {
            trig_ok = clock_trig_on();
        }
    }
    trace_point("clock_trig_on() (CLKGEN13)");
    ok = sccp1_start(100u, SCCP_CLK_GEN13, SCCP_MODE_TIMER, SCCP_EVENT_SPECIAL);
    trace_note("# sccp1_start(GEN13, TIMER, SPECIAL) -> %d\n", (int)ok);
    trace_point("T_G13 (= TRG2): CLKGEN13, timer mode, special event trigger");

    ok = sccp1_start(100u, SCCP_CLK_PERIPHERAL, SCCP_MODE_OC, SCCP_EVENT_SPECIAL);
    trace_note("# sccp1_start(PERIPHERAL, OC, SPECIAL) -> %d\n", (int)ok);
    trace_point("OC_PER: peripheral clock, output compare, special event trigger");

    ok = sccp1_start(100u, SCCP_CLK_GEN13, SCCP_MODE_OC, SCCP_EVENT_SPECIAL);
    trace_note("# sccp1_start(GEN13, OC, SPECIAL) -> %d\n", (int)ok);
    trace_point("OC_G13: CLKGEN13, output compare, special event trigger");

    ok = sccp1_start(100u, SCCP_CLK_PERIPHERAL, SCCP_MODE_TIMER, SCCP_EVENT_ROLLOVER);
    trace_note("# sccp1_start(PERIPHERAL, TIMER, ROLLOVER) -> %d\n", (int)ok);
    trace_point("OLD: peripheral clock, timer mode, rollover (runs 5-7's failing combination)");

    sccp1_stop();
    trace_point("sccp1_stop()");

    hwmodel_stop();
    trace_end();
    return 0;
}

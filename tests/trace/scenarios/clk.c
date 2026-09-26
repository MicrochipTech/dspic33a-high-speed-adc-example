/*
 * clk.c (scenario, P0.5) - the run-time rate controls: capture_set_clkdiv()
 * (capture.c's wrapper over clock_adc_set_div(), kept "for the record and
 * for the clk command", clock.c) and clock_adc_set_rate() for three rates
 * (4000, 8000, 40000 ksps: the low end, the customer's floor, and the
 * high end of its 4000..40000 range).
 *
 * Not clock_init() (that is `boot`'s and the P0.4 `clock` scenario's job):
 * both functions here only touch CLK6DIV/PLL1DIV's own fields and the
 * switches that apply them, starting from whatever clock_init() would
 * have left running - which this scenario does not need to have happened
 * first, since neither function reads a *rate* out of prior state, only
 * out of the ratio/ksps argument it is given.
 */
#include <setjmp.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "capture.h"
#include "clock.h"

extern jmp_buf fail_jmp;

/* Same shape as `b2b`: PLL1CON/OSCCTRL/CLK6CON answer clock_adc_set_rate()'s
 * waits (PLLSWEN, FOUTSWEN, PLL1RDY, CLKRDY); CLK6CON alone answers
 * capture_set_clkdiv()'s (DIVSWEN, CLKRDY). AD3CON.ADRDY: the board's
 * default core, which both functions take down and back up around the
 * clock change. */
static const hwmodel_rule_t rules[] = {
    { "PLL1CON", _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL", 0u, _OSCCTRL_PLL1RDY_MASK },
    { "CLK6CON", _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "AD3CON",  0u, _AD3CON_ADRDY_MASK },
};

int main(void)
{
    trace_begin("clk");
    hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);

    /* Every call below runs exactly once (P0.5b, "the hybrid") - the
     * page-guard read hook answers these functions' CLK6CON/PLL1CON/
     * OSCCTRL waits deterministically; see the `dac` scenario and
     * tests/trace/README.md. */
    uint32_t rc = CLKDIV_RANGE;
    if (setjmp(fail_jmp) == 0) {
        rc = capture_set_clkdiv(500u);        /* 8 MSPS via the CLKGEN6 divider */
        trace_note("# capture_set_clkdiv(500) -> %lu\n", (unsigned long)rc);
    } else {
        trace_note("# capture_set_clkdiv(500) called fail() - see the F line above\n");
    }
    trace_point("capture_set_clkdiv(500)");

    uint32_t got = 0u;
    rc = CLKDIV_RANGE;
    if (setjmp(fail_jmp) == 0) {
        rc = clock_adc_set_rate(4000u, &got);
        trace_note("# clock_adc_set_rate(4000, &got) -> rc=%lu got=%lu\n", (unsigned long)rc, (unsigned long)got);
    } else {
        trace_note("# clock_adc_set_rate(4000) called fail() - see the F line above\n");
    }
    trace_point("clock_adc_set_rate(4000)");

    rc = CLKDIV_RANGE;
    if (setjmp(fail_jmp) == 0) {
        rc = clock_adc_set_rate(8000u, &got);
        trace_note("# clock_adc_set_rate(8000, &got) -> rc=%lu got=%lu\n", (unsigned long)rc, (unsigned long)got);
    } else {
        trace_note("# clock_adc_set_rate(8000) called fail() - see the F line above\n");
    }
    trace_point("clock_adc_set_rate(8000)");

    rc = CLKDIV_RANGE;
    if (setjmp(fail_jmp) == 0) {
        rc = clock_adc_set_rate(40000u, &got);
        trace_note("# clock_adc_set_rate(40000, &got) -> rc=%lu got=%lu\n", (unsigned long)rc, (unsigned long)got);
    } else {
        trace_note("# clock_adc_set_rate(40000) called fail() - see the F line above\n");
    }
    trace_point("clock_adc_set_rate(40000)");

    hwmodel_stop();
    trace_end();
    return 0;
}

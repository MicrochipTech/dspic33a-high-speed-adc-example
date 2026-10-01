/*
 * stream_on_input.c (scenario, P0.5) - chain_stream_on_input() with a
 * non-default core and pin: core 2, pinsel 7, samc 1 - none of them
 * CHAIN_CORE/CHAIN_PINSEL/CHAIN_SAMC (5/3/0) - and test_signal = false,
 * the shape `stream on <ksps> <core> <pinsel> [<samc>]` uses for a custom
 * ADC input rather than the built-in DAC2 triangle (CLAUDE.md, "The GUI's
 * chain stream cycle": "the DAC is then left alone (slp=0 in the frame)").
 *
 * With test_signal = false, setup() skips dac2_level_start() entirely
 * (`setup_dac = s_test_dac ? dac2_level_start(0x800u) : true;`) and
 * chain_stream_on_input() skips triangle_for() too - so CLKGEN7 (the DAC
 * clock) is never touched here, unlike `stream_on`/`dac`. clock_trig_on()
 * (CLKGEN13) still runs unconditionally inside setup(), regardless of
 * test_signal. capture_select_core(2, 7, 1) needs AD2CON.ADRDY; restore()
 * switches back to ADC_INSTANCE (3), needing AD3CON.ADRDY, exactly as in
 * `stream_on`. See stream_on.c for the rest of the call chain, the TMR1
 * stepping rationale and the PLL1DIV/VCO1DIV preset (still needed: setup()
 * still calls capture_set_pll(5, 1), which only rewrites POSTDIV1/2).
 */
#include <setjmp.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "capture.h"
#include "chaintest.h"

extern jmp_buf fail_jmp;

static const hwmodel_rule_t rules[] = {
    { "PLL1CON",  _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL",  0u, _OSCCTRL_PLL1RDY_MASK },
    { "CLK6CON",  _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "CLK13CON", _CLK13CON_OSWEN_MASK | _CLK13CON_DIVSWEN_MASK, _CLK13CON_CLKRDY_MASK },
    { "AD2CON",   0u, _AD2CON_ADRDY_MASK },   /* the non-default core asked for */
    { "AD3CON",   0u, _AD3CON_ADRDY_MASK },   /* restore(): back to ADC_INSTANCE = 3 */
};

int main(void)
{
    trace_begin("stream_on_input");
    trace_region(capture_buffer(), SAMPLES_PER_ALLOC * sizeof(uint16_t), "dma_buffer");   /* both ping-pong pairs */

    PLL1DIV = 0x0100C829u;
    VCO1DIV = 0x20000u;
    trace_point("preset: PLL1DIV/VCO1DIV as clock_init() leaves them at boot");

    hwmodel_start(rules, sizeof rules / sizeof rules[0], 10000u);

    /* Called exactly once (P0.5b, "the hybrid") - see stream_on.c's
     * identical comment. */
    bool ok = false;
    if (setjmp(fail_jmp) == 0) {
        ok = chain_stream_on_input(1000u, 2u, 7u, 1u, false);
        trace_note("# chain_stream_on_input(1000, core=2, pinsel=7, samc=1, test_signal=false) -> %d\n", (int)ok);
    } else {
        trace_note("# chain_stream_on_input(...) called fail() - see the F line above\n");
    }
    trace_point("chain_stream_on_input(1000, 2, 7, 1, false)");

    chain_stream_off();
    trace_point("chain_stream_off()");

    hwmodel_stop();
    trace_end();
    return 0;
}

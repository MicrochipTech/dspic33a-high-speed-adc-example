/*
 * route_stream.c (scenario, P11.3, 27.09.2026) - routing_apply(&ROUTE_STREAM)
 * followed by exactly what chain_stream_on_input() does after its own
 * setup call at 1 MSPS, then the stop/restore chain_stream_off() runs. The
 * point: the register-write sequence must equal the `stream_on` golden's,
 * line for line, apart from the `#` note lines the two scenarios write
 * themselves - that is the proof P11.3 owes before P11.4 routes "stream on"
 * through routing_apply() (docs/IMPLEMENTATION-PLAN.md P11.3/P11.4).
 *
 * What is under test is the FIRST block only, up to the trace_point
 * "routing_apply(&ROUTE_STREAM)": route_check() (no register), then
 * acq_chain_setup_input(5, 3, 0, true) -> acq_chain_setup() - capture_set_
 * pll(5,1), clock_trig_on() (CLKGEN13), clock_dac_select()+dac2_level_start()
 * (CLKGEN7), capture_select_core(5, 3, 0) (full adc_init() + capture_init()
 * on core 5), adc_set_mode_single()/adc_set_irqsel(0), capture_settle() - the
 * same calls `stream_on`'s header lists for setup(). In the `stream_on`
 * golden that block ends at "W AD5CH0CON1 ... -> 0x03000020" followed by the
 * second "[dma] channel 0 armed" report (capture_settle() taking the channel
 * down again).
 *
 * The SECOND block is not routing's: the rate is not part of a route
 * (routing.h, requirement A1), so chain_stream_on_input() starts the shared
 * trigger itself after the setup - period_for(1000) = 160 ticks of CLKGEN13
 * at the nominal 160 MHz, acq_triangle_for(acq_rate_hz(160), &slp) (the DAC2
 * triangle for the test signal), acq_wait_ticks(TICKS_PER_MS), capture_chain_
 * start(160, SCCP_MODE_TIMER, 0, false). Those four calls are replayed here
 * verbatim (period_for() is static in acquisition.c; 160 is what it returns
 * for 1000 kSPS: (160000 + 500) / 1000 = 160) so that the golden can be
 * compared against `stream_on` as a whole, not only its first half. The
 * THIRD block is chain_stream_off()'s body - capture_chain_stop(),
 * acq_chain_restore() - plus routing_clear() (chain_stream_off() cannot be
 * called: it checks acquisition.c's own s_on flag, which nothing here set).
 *
 * Result (27.09.2026, the golden committed with P11.3): every W and C line
 * equals `stream_on`'s except SIX extra W lines around the trace_point that
 * closes block 1 - IEC2/DMACON/DMA0CH going down (0x2000->0, 0x8001->1,
 * ..4B->..4A) right before it and back up right after. Both scenarios
 * perform those writes: acq_chain_setup()'s final capture_settle() takes
 * the channel down and capture_chain_start() re-arms it. `stream_on` has no
 * snapshot between the two (chain_stream_on() is one call), so its golden
 * shows the pair as nothing - the same blind spot tests/trace/README.md
 * records for a reset-value write ("What the trace cannot see"). Checked
 * both ways: a throwaway copy of this scenario without block 1's
 * trace_note/trace_point produced W/C lines byte-identical to `stream_on`
 * (tools\trace.bat record, diff, not committed). The snapshot point is kept
 * here on purpose: it is what marks where routing_apply() ends, and it
 * makes the DMA-down/up pair visible for once.
 *
 * The note "slp=0": acq_triangle_for() refuses the triangle in the harness
 * (clock_dac_hz() switches on CLK7CON.COSC, which the model never moves
 * from its reset value), so no DAC2SLPCON/DAC2SLPDAT write appears - nor
 * does one in `stream_on`'s golden, whose chain_stream_on(1000) hits the
 * same refusal. Identical behaviour, so nothing to fix here; the `dac`
 * scenario traces the triangle registers themselves.
 *
 * chaintest.c is NOT linked (unlike `stream_on`): nothing here calls into
 * it since P9.4b, so the closure is `clk`'s plus routing.c - and no
 * -DHAVE_CHAINTEST, so adc.c's `adc_ch0_event()` resolves to stubs.c's stub
 * (never reached: no ISR fires, decision 2). Everything else - the model
 * rules, the PLL1DIV/VCO1DIV preset, tmr1_step - is `stream_on`'s, for the
 * same reasons its header gives.
 */
#include <setjmp.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "capture.h"
#include "routing.h"
#include "sccp.h"
#include "timebase.h"

extern jmp_buf fail_jmp;

static const hwmodel_rule_t rules[] = {
    { "PLL1CON",  _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL",  0u, _OSCCTRL_PLL1RDY_MASK },
    { "CLK6CON",  _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "CLK7CON",  _CLK7CON_OSWEN_MASK, _CLK7CON_CLKRDY_MASK },
    { "CLK13CON", _CLK13CON_OSWEN_MASK | _CLK13CON_DIVSWEN_MASK, _CLK13CON_CLKRDY_MASK },
    { "AD5CON",   0u, _AD5CON_ADRDY_MASK },   /* ROUTE_STREAM.core = CHAIN_CORE = 5 */
    { "AD3CON",   0u, _AD3CON_ADRDY_MASK },   /* acq_chain_restore(): back to ADC_INSTANCE = 3 */
};

/* chain_stream_on_input()'s period_for(1000): (160000 + 500) / 1000. */
#define TICKS_1MSPS     160u
/* acquisition_priv.h's TICKS_PER_MS, not included here (it is acquisition.c's
 * and chaintest.c's private header): TIMEBASE_HZ / 1000. */
#define TICKS_PER_MS_   (TIMEBASE_HZ / 1000u)

int main(void)
{
    trace_begin("route_stream");
    trace_region(capture_buffer(), SAMPLES_PER_BUF_MAX * sizeof(uint16_t), "dma_buffer");

    /* Preset: PLL1 as clock_init() leaves it at boot (see file header). */
    PLL1DIV = 0x0100C829u;
    VCO1DIV = 0x20000u;
    trace_point("preset: PLL1DIV/VCO1DIV as clock_init() leaves them at boot");

    hwmodel_start(rules, sizeof rules / sizeof rules[0], 10000u);

    /* Block 1 - what P11.3 owns. Called exactly once (P0.5b): the read
     * hook answers acq_chain_setup()'s PLL/CLKGEN7/CLKGEN13 waits. */
    route_err_t e = ROUTE_ERR_SETUP;
    if (setjmp(fail_jmp) == 0) {
        e = routing_apply(&ROUTE_STREAM);
        trace_note("# routing_apply(&ROUTE_STREAM) -> %d (0 = ROUTE_OK)\n", (int)e);
    } else {
        trace_note("# routing_apply(&ROUTE_STREAM) called fail() - see the F line above\n");
    }
    trace_point("routing_apply(&ROUTE_STREAM)");

    /* Block 2 - chain_stream_on_input()'s tail at 1000 kSPS, replayed. */
    if (e == ROUTE_OK) {
        uint16_t slp = 0u;
        (void)acq_triangle_for(acq_rate_hz(TICKS_1MSPS), &slp);
        acq_wait_ticks(TICKS_PER_MS_);
        const bool started = capture_chain_start(TICKS_1MSPS, SCCP_MODE_TIMER, 0u, false);
        trace_note("# chain start at 1000 kSPS as chain_stream_on_input() does -> %d, slp=%u\n",
                   (int)started, (unsigned)slp);
    }
    trace_point("chain_stream_on_input()'s tail: triangle, wait, capture_chain_start(160)");

    /* Block 3 - chain_stream_off()'s body. */
    (void)capture_chain_stop();
    acq_chain_restore();
    routing_clear();
    trace_point("capture_chain_stop() + acq_chain_restore(), as chain_stream_off() does");

    hwmodel_stop();
    trace_end();
    return 0;
}

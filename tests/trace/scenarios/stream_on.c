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
 * stream_on.c (scenario, P0.5) - chain_stream_on(1000) (1 MSPS) and
 * chain_stream_off(), chaintest.c compiled unchanged - this is the GUI's
 * only capture path (tests/trace/../../CLAUDE.md, "The GUI's chain stream
 * cycle"), the deepest call chain any P0.5 scenario drives.
 *
 * chain_stream_on(1000) -> chain_stream_on_input(1000, CHAIN_CORE=5,
 * CHAIN_PINSEL=3, CHAIN_SAMC=0, test_signal=true) -> setup(): capture_set_pll
 * (5,1), clock_trig_on() (CLKGEN13), clock_dac_select()+dac2_level_start()
 * (CLKGEN7), capture_select_core(5, 3, 0) (full adc_init() + capture_init()
 * on core 5), adc_set_mode_single()/adc_set_irqsel(0), capture_settle() -
 * then triangle_for(rate_hz(160), &slp) (test_signal true: the DAC2
 * triangle, same arithmetic as the `dac` scenario, at a different rate),
 * wait_ticks(TICKS_PER_MS), capture_chain_start(160, SCCP_MODE_TIMER, 0,
 * false) (sccp1_start() on CLKGEN13). chain_stream_off() -> capture_chain_
 * stop() (sccp1_stop(), a 25-tick wait, capture_settle()) -> restore()
 * (capture_select_core back to ADC_INSTANCE=3/ADC_PINSEL/ADC_SAMC,
 * capture_set_pll(7,7), counters_clear()).
 *
 * wait_ticks() (chaintest.c) and the two 25-Timer1-tick waits in capture.c
 * are genuine SOFTWARE waits on TMR1, UNBOUNDED (no WAIT_LIMIT, no fail())
 * - the open point tests/trace/README.md flagged as "TMR1 as a continuous
 * count ... not exercised by either P0.4 scenario". hwmodel_start()'s
 * tmr1_step parameter (already in hwmodel.h since P0.4, unused until now)
 * answers it: TMR1 advances every model iteration regardless of the
 * driver thread's schedule, so every wait here terminates at a value that
 * depends only on the wait's own bound and the step size - not on timing -
 * which is what keeps it deterministic (same reasoning as the
 * level-triggered SFR rules, tests/trace/README.md's hardware-model
 * section).
 *
 * PLL1DIV/VCO1DIV are preset exactly as the `dac` scenario does, for the
 * same reason: capture_set_pll(5,1) only rewrites POSTDIV1/POSTDIV2, and
 * clock_dac_hz() (which triangle_for() depends on through clock_dac_on())
 * needs PLLFBDIV/VCO1DIV already at their clock_init() boot values, or it
 * reads 0 and refuses the triangle. Re-tracing clock_init() itself is
 * `boot`'s and `clock`'s job, not this scenario's.
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
    { "CLK7CON",  _CLK7CON_OSWEN_MASK, _CLK7CON_CLKRDY_MASK },
    { "CLK13CON", _CLK13CON_OSWEN_MASK | _CLK13CON_DIVSWEN_MASK, _CLK13CON_CLKRDY_MASK },
    { "AD5CON",   0u, _AD5CON_ADRDY_MASK },   /* CHAIN_CORE = DAC_ADC_CORE = 5 */
    { "AD3CON",   0u, _AD3CON_ADRDY_MASK },   /* restore(): back to ADC_INSTANCE = 3 */
};

int main(void)
{
    trace_begin("stream_on");
    trace_region(capture_buffer(), SAMPLES_PER_ALLOC * sizeof(uint16_t), "dma_buffer");   /* both ping-pong pairs */

    /* Preset: PLL1 as clock_init() leaves it at boot (see file header). */
    PLL1DIV = 0x0100C829u;
    VCO1DIV = 0x20000u;
    trace_point("preset: PLL1DIV/VCO1DIV as clock_init() leaves them at boot");

    hwmodel_start(rules, sizeof rules / sizeof rules[0], 10000u);

    /* Called exactly once (P0.5b, "the hybrid") - the page-guard read hook
     * answers setup()'s PLL/CLKGEN7/CLKGEN13 waits deterministically; see
     * the `dac` scenario and tests/trace/README.md. */
    bool ok = false;
    if (setjmp(fail_jmp) == 0) {
        ok = chain_stream_on(1000u);
        trace_note("# chain_stream_on(1000) -> %d\n", (int)ok);
    } else {
        trace_note("# chain_stream_on(1000) called fail() - see the F line above\n");
    }
    trace_point("chain_stream_on(1000)");

    chain_stream_off();
    trace_point("chain_stream_off()");

    hwmodel_stop();
    trace_end();
    return 0;
}

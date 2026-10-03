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
 * dac.c (scenario, P0.5) - the DAC2 triangle exactly as "chain all" sets
 * it (chaintest.c stage8, `triangle_for(8000000u, &slp)`), and dac2 off.
 *
 * triangle_for() itself is `static` in chaintest.c and not reachable from
 * here without linking the whole chain test (out of scope for this
 * scenario - dac.c and clock.c are all it needs). Its arithmetic is
 * reproduced instead, for rate = 8 000 000 (8 MSPS, the same call site):
 *
 *   full = DAC_CODE_MAX - DAC_CODE_MIN - 64 = 0xF32 - 0xCD - 64 = 3621
 *   the loop's first s with (full - 2s) * 32 * rate / (s * f) <= 128
 *   (SLOPE_TARGET), f = clock_dac_hz() = 400 000 000 (PLL1 VCO / 2):
 *   s = 18 gives 3585 * 0.64 / 18 = 127.47 <= 128; s = 17 gives 135.06 -
 *   too big. So slp = 18, low = DAC_CODE_MIN + s + 32 = 0xFF,
 *   high = DAC_CODE_MAX - s - 32 = 0xF00 - the values dac2_triangle_start()
 *   below is called with.
 *
 * clock_dac_hz() (clock.c) reads PLL1DIV/VCO1DIV, not a stored rate - so it
 * only answers 400 MHz once those registers hold what clock_init() leaves
 * them at boot (PLL1DIV = 0x0100C829, VCO1DIV = 0x20000, both traced in
 * detail by the `clock`/`boot` scenarios already). Re-deriving that
 * through a full clock_init() here would re-trace clock_init() a third
 * time for a scenario that is about the DAC, not the clock tree, so the
 * two registers are preset directly (as `boot`'s golden trace records
 * clock_init() leaving them) - the "as chain all sets it" triangle is
 * only reachable with clock_dac_hz() non-zero, and this is the smallest
 * way to get there without re-testing clock_init(). Documented, not
 * hidden: the preset shows up as ordinary W lines in the trace.
 *
 * clock.c's object file references capture_halt(), console_force_up() and
 * boot_stage from _CLKFInterrupt (never called here) and clock_monitor_hz()
 * needs timebase_ticks() - hence clock.c pulls in timebase.c, and
 * capture_halt()/console_force_up() fall back to the stubs in stubs.c
 * (capture.c is not linked - this scenario has no DMA/ADC in it at all).
 */
#include <stdbool.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "clock.h"
#include "dac.h"

static const hwmodel_rule_t rules[] = {
    /* clock_dac_on() (dac.c, via dac2_triangle_start()): OSWEN self-clears,
     * CLKRDY is hardware-set once the generator is running - the same
     * shape as CLK6CON in the `clock` scenario, for CLKGEN7 (the DAC
     * clock) instead of CLKGEN6. Open point closed: tests/trace/README.md
     * listed "CLK7/13CON not exercised" under clock.c's CLKRDY sites. */
    { "CLK7CON", _CLK7CON_OSWEN_MASK, _CLK7CON_CLKRDY_MASK },
};

int main(void)
{
    trace_begin("dac");

    /* Preset: PLL1 as clock_init() leaves it at boot (see file header) -
     * only what clock_dac_hz() needs (PLL1DIVbits.PLLPRE/PLLFBDIV,
     * VCO1DIVbits.INTDIV), not a re-run of clock_init() itself. */
    PLL1DIV = 0x0100C829u;
    VCO1DIV = 0x20000u;
    trace_point("preset: PLL1DIV/VCO1DIV as clock_init() leaves them at boot");

    hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);

    clock_dac_select(CLOCK_DAC_PLL1_VCO);
    trace_point("clock_dac_select(CLOCK_DAC_PLL1_VCO)");

    /* The exact triangle chaintest.c's stage8 sets up at 8 MSPS - see the
     * file header for the arithmetic. Called exactly once (P0.5b, "the
     * hybrid"): the page-guard read hook answers clock_dac_on()'s CLK7CON
     * wait deterministically, on the first read - see hwmodel.c and
     * tests/trace/README.md. */
    bool ok = dac2_triangle_start(0x00FFu, 0x0F00u, 18u);
    trace_note("# dac2_triangle_start(0xFF, 0xF00, 18) -> %d\n", (int)ok);
    trace_point("after dac2_triangle_start()");

    dac2_off();
    trace_point("after dac2_off()");

    hwmodel_stop();
    trace_end();
    return 0;
}

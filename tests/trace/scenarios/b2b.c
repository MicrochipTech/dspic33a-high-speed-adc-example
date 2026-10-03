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
 * b2b.c (scenario, P0.5) - the back-to-back path's own control calls,
 * capture.c compiled unchanged: capture_set_pll(5,5) (8 MSPS, the
 * customer's floor), capture_start(), capture_stop().
 *
 * capture_start() calls capture_init() itself the first time (dma_armed
 * starts false), so this scenario also exercises dma0_init() without a
 * separate call to it. capture_stop() only clears run_enabled - there is
 * no ISR in this harness to actually finish the burst capture_start()
 * begins (decision 2, 26.09.2026: no ISR-firing hook), so this traces the
 * three control calls, not a completed capture.
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

/* capture_set_pll() -> clock_adc_set_pll(): PLL1CON.FOUTSWEN self-clears,
 * OSCCTRL.PLL1RDY and CLK6CON.CLKRDY are hardware-set - the same shape as
 * the `clock`/`boot` scenarios' rules, minus PLL2CON/CLK1CON (this path
 * never touches the system clock). AD3CON.ADRDY: board default core
 * (ADC_INSTANCE 3), needed by capture_start()'s adc_reinit() path via
 * clock_adc_set_pll()'s own adc_deinit()/adc_reinit() pair inside
 * capture_set_pll(). */
static const hwmodel_rule_t rules[] = {
    { "PLL1CON", _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL", 0u, _OSCCTRL_PLL1RDY_MASK },
    { "CLK6CON", _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "AD3CON",  0u, _AD3CON_ADRDY_MASK },
};

int main(void)
{
    trace_begin("b2b");
    trace_region(capture_buffer(), SAMPLES_PER_ALLOC * sizeof(uint16_t), "dma_buffer");   /* both ping-pong pairs */
    hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);

    /* Called exactly once (P0.5b, "the hybrid") - the page-guard read hook
     * answers clock_adc_set_pll()'s waits deterministically; see the
     * `dac` scenario and tests/trace/README.md. */
    uint32_t rc = CLKDIV_RANGE;
    if (setjmp(fail_jmp) == 0) {
        rc = capture_set_pll(5u, 5u);
        trace_note("# capture_set_pll(5, 5) -> %lu\n", (unsigned long)rc);
    } else {
        trace_note("# capture_set_pll(5, 5) called fail() - see the F line above\n");
    }
    trace_point("capture_set_pll(5, 5)");

    if (setjmp(fail_jmp) == 0) {
        capture_start();
        trace_note("# capture_start() returned\n");
    } else {
        trace_note("# capture_start() called fail() - see the F line above\n");
    }
    trace_point("capture_start()");

    capture_stop();
    trace_point("capture_stop()");

    hwmodel_stop();
    trace_end();
    return 0;
}

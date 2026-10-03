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
 * boot.c (scenario, P0.5) - main()'s start-up order up to capture_init(),
 * every module compiled unchanged: led_init(), console_early_init(),
 * clock_init(), cli_init(), timebase_init(), adc_init(), capture_init() -
 * the plan's entry point list, called in the same order main.c uses them
 * (docs/IMPLEMENTATION-PLAN.md's table lists them alphabetically by
 * module, not by call order; main.c's own order is what is traced).
 *
 * console_early_init() and cli_init() both live in cli.c, which decision 1
 * (26.09.2026, tests/trace/README.md) excludes from every scenario without
 * exception - both resolve to the plain stubs in stubs.c (a `C` line each,
 * "stub: cli.c not linked"). Their real effect - the console pins, PPS and
 * FRC baud generator for console_early_init(); the PLL baud generator and
 * ~26 cmd_parser command registrations for cli_init() - is not in this or
 * any other P0.5 golden trace. capture_halt() and console_force_up(), the
 * two callees clock.c's _CLKFInterrupt references (never called here),
 * DO get their real definitions further down the object list where
 * capture.c is linked (capture_halt()) - console_force_up() never does,
 * for the same cli.c reason.
 *
 * boot_mark(), diag_report_reset(), crc16_selfcheck() and the console
 * banner text main() prints between these calls are not in the plan's
 * entry point list and are left out - they are boot-order glue and a
 * CRC self-check, not hardware state this harness can add anything to.
 */
#include <setjmp.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "board.h"
#include "console.h"
#include "clock.h"
#include "adc.h"
#include "capture.h"
#include "led.h"
#include "timebase.h"

extern jmp_buf fail_jmp;

/* Every self-clearing switch-enable bit and hardware-set ready bit
 * clock_init() waits on (identical to the `clock` scenario's rule set,
 * tests/trace/scenarios/clock.c) plus this board's ADC core (ADC_INSTANCE
 * = 3, board.h): ADxCON.ADRDY, hardware-set once the core is switched on -
 * the open point tests/trace/README.md's polling table listed as
 * "adc.c 131, 287 | ADxCON.ADRDY | set by hardware after ON | open (no ADC
 * scenario in P0.4)". `boot` is that scenario. */
static const hwmodel_rule_t rules[] = {
    { "PLL1CON", _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "PLL2CON", _PLL2CON_PLLSWEN_MASK | _PLL2CON_FOUTSWEN_MASK | _PLL2CON_OSWEN_MASK | _PLL2CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL", 0u, _OSCCTRL_PLL1RDY_MASK | _OSCCTRL_PLL2RDY_MASK },
    { "CLK1CON", _CLK1CON_OSWEN_MASK | _CLK1CON_DIVSWEN_MASK, _CLK1CON_CLKRDY_MASK },
    { "CLK6CON", _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "AD3CON",  0u, _AD3CON_ADRDY_MASK },
};

int main(void)
{
    trace_begin("boot");
    /* Registered before anything runs: capture_init() below sets DMALOW/
     * DMAHIGH/DMA0DST to this buffer's address, and this is what turns
     * those from a raw (if deterministic) host pointer into "&dma_buffer+
     * 0xNNN" in the trace - tests/trace/README.md's open point on
     * capture.c's static dma_buffer, closed via the public capture_buffer()
     * accessor rather than a linker-script symbol into a private section. */
    trace_region(capture_buffer(), SAMPLES_PER_ALLOC * sizeof(uint16_t), "dma_buffer");   /* both ping-pong pairs */

    hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);

    led_init();
    trace_point("led_init()");

    console_early_init();
    trace_point("console_early_init()");

    if (setjmp(fail_jmp) == 0) {
        clock_init();
        trace_note("# clock_init() returned\n");
    } else {
        trace_note("# clock_init() called fail() - see the F line above\n");
    }
    trace_point("clock_init()");

    cli_init();
    trace_point("cli_init()");

    timebase_init();
    trace_point("timebase_init()");

    adc_init(ADC_PINSEL, ADC_SAMC, SAMPLES_PER_BUF_MAX);
    trace_point("adc_init()");

    capture_init();
    trace_point("capture_init()");

    hwmodel_stop();
    trace_end();
    return 0;
}

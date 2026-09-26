/*
 * nano.c (scenario, P0.5) - `boot`'s exact sequence, built with
 * -DBOARD=2 (BOARD_EV17P63A, dsPIC33AK512MPS506 Curiosity Nano,
 * nano.cflags) against a fake xc.h generated from THAT device's own
 * header (nano.mcu = 33AK512MPS506, tools/trace.bat, tools/gen_fake_sfr.py
 * --mcu, which already took a device argument since P0.3 - no generator
 * change needed). board.h's BOARD_EV17P63A branch changes ADC_INSTANCE
 * (1, not 3), ADC_PINSEL (0, not 5), the LED pin/polarity and the console
 * pin routing/RP numbers; the register SET is the same on both devices
 * (CLAUDE.md: "every register and vector core 5 uses is identical on the
 * MPS506, checked against the pack header 25.09.2026"), only which
 * instance and which pins are named differs - which is exactly what a
 * second golden trace, side by side with `boot`'s, is for.
 *
 * Otherwise identical to boot.c; see its comments for what is stubbed and
 * why. The only functional difference here is which ADC core's ADRDY the
 * hardware model answers: AD1CON, board.h's ADC_INSTANCE for BOARD_EV17P63A.
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

static const hwmodel_rule_t rules[] = {
    { "PLL1CON", _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "PLL2CON", _PLL2CON_PLLSWEN_MASK | _PLL2CON_FOUTSWEN_MASK | _PLL2CON_OSWEN_MASK | _PLL2CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL", 0u, _OSCCTRL_PLL1RDY_MASK | _OSCCTRL_PLL2RDY_MASK },
    { "CLK1CON", _CLK1CON_OSWEN_MASK | _CLK1CON_DIVSWEN_MASK, _CLK1CON_CLKRDY_MASK },
    { "CLK6CON", _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
    { "AD1CON",  0u, _AD1CON_ADRDY_MASK },   /* BOARD_EV17P63A: ADC_INSTANCE 1 */
};

int main(void)
{
    trace_begin("nano");
    trace_region(capture_buffer(), SAMPLES_PER_BUF_MAX * sizeof(uint16_t), "dma_buffer");

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

    adc_init(ADC_PINSEL, ADC_SAMC);
    trace_point("adc_init()");

    capture_init();
    trace_point("capture_init()");

    hwmodel_stop();
    trace_end();
    return 0;
}

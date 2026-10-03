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
 * clock.c (scenario) - P0.4 working example: clock_init(), clock.c
 * compiled unchanged, run against the hardware model (hwmodel.c) that
 * answers every self-clearing switch-enable bit and hardware-set ready
 * bit clock_init() waits on (tests/trace/README.md's polling-loop
 * table: PLL1CON/PLL2CON's PLLSWEN/FOUTSWEN/OSWEN/DIVSWEN, OSCCTRL's
 * PLL1RDY/PLL2RDY, CLK1CON/CLK6CON's OSWEN). This proves the model, not
 * a golden trace yet (P0.5).
 *
 * TRACE_HWMODEL=0 in the environment skips starting the model: then
 * clock_init() runs PLL1CON.PLLSWEN's wait into its WAIT_LIMIT bound and
 * fail(1)s on the very first one - the evidence that the model, not
 * something else (e.g. the reset value happening to already satisfy the
 * wait), is what lets clock_init() complete.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "clock.h"

extern jmp_buf fail_jmp;

static const hwmodel_rule_t rules[] = {
    { "PLL1CON", _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "PLL2CON", _PLL2CON_PLLSWEN_MASK | _PLL2CON_FOUTSWEN_MASK | _PLL2CON_OSWEN_MASK | _PLL2CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL", 0u, _OSCCTRL_PLL1RDY_MASK | _OSCCTRL_PLL2RDY_MASK },
    { "CLK1CON", _CLK1CON_OSWEN_MASK | _CLK1CON_DIVSWEN_MASK, _CLK1CON_CLKRDY_MASK },
    { "CLK6CON", _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
};

int main(void)
{
    const char *e = getenv("TRACE_HWMODEL");
    int use_model = !(e != NULL && e[0] == '0');

    trace_begin("clock");
    trace_note("# hardware model %s\n", use_model ? "ON" : "OFF");
    if (use_model) {
        hwmodel_start(rules, sizeof rules / sizeof rules[0], 0u);
    }

    trace_point("clock_init()");
    if (setjmp(fail_jmp) == 0) {
        clock_init();
        trace_note("# clock_init() returned\n");
    }
    trace_point("after clock_init()");

    if (use_model) {
        hwmodel_stop();
    }
    trace_end();
    return 0;
}

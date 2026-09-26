/* spike_poll.c - P0.3 spike: polling loops on self-clearing and
 * hardware-set bits, with clock.c compiled unchanged.
 *
 * clock_init() writes PLLSWEN/FOUTSWEN/OSWEN/DIVSWEN = 1 and waits for the
 * hardware to clear them, and waits for OSCCTRL.PLLxRDY and CLKxCON.CLKRDY
 * to be set. A plain variable keeps a 1 the driver wrote forever, so the
 * wait runs into its bound and fail() - unless a read hook plays hardware.
 * Built with TRACE_MODE 3 (page guard). POLL_HOOKS=0 in the environment
 * leaves the hooks out, to show what a snapshot-only harness gets. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "clock.h"
#include "timebase.h"

extern jmp_buf fail_jmp;

/* What clock.c needs besides dma.c/timebase.c's stubs (stubs.c). */
void console_flush(void)                    { trace_note("C <flush>\n"); }
void console_force_up(void)                 { trace_note("C <force_up>\n"); }
void console_kv(const char *k, uint32_t v)  { trace_note("C %s: %lu\r\n", k, (unsigned long)v); }
void capture_halt(void)                     { trace_note("D capture_halt()\n"); }
volatile uint32_t boot_stage;               /* diag.c */

/* Read hooks: on every read the "hardware" has finished whatever was
 * requested - switch-enable bits clear themselves, ready bits are set. */
typedef struct { const char *reg; uint32_t clear; uint32_t set; } hw_rule_t;
static const hw_rule_t rules[] = {
    { "PLL1CON", _PLL1CON_PLLSWEN_MASK | _PLL1CON_FOUTSWEN_MASK | _PLL1CON_OSWEN_MASK | _PLL1CON_DIVSWEN_MASK, 0u },
    { "PLL2CON", _PLL2CON_PLLSWEN_MASK | _PLL2CON_FOUTSWEN_MASK | _PLL2CON_OSWEN_MASK | _PLL2CON_DIVSWEN_MASK, 0u },
    { "OSCCTRL", 0u, _OSCCTRL_PLL1RDY_MASK | _OSCCTRL_PLL2RDY_MASK },
    { "CLK1CON", _CLK1CON_OSWEN_MASK | _CLK1CON_DIVSWEN_MASK, _CLK1CON_CLKRDY_MASK },
    { "CLK6CON", _CLK6CON_OSWEN_MASK | _CLK6CON_DIVSWEN_MASK, _CLK6CON_CLKRDY_MASK },
};
static unsigned rule_idx[sizeof rules / sizeof rules[0]];

static void hw_hook(unsigned idx)
{
    for (unsigned r = 0; r < sizeof rules / sizeof rules[0]; r++) {
        if (rule_idx[r] == idx) {
            uint32_t v = hw_get(idx);
            hw_set(idx, (v & ~rules[r].clear) | rules[r].set);
        }
    }
}

static void tmr1_hook(unsigned idx) { hw_set(idx, hw_get(idx) + 1u); }

int main(void)
{
    const char *e = getenv("POLL_HOOKS");
    int hooks = !(e != NULL && e[0] == '0');
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    trace_init();
    trace_hook(trace_idx("TMR1"), tmr1_hook);
    if (hooks) {
        for (unsigned r = 0; r < sizeof rules / sizeof rules[0]; r++) {
            rule_idx[r] = trace_idx(rules[r].reg);
            trace_hook(rule_idx[r], hw_hook);
        }
    }
    printf("# clock.c polling, read hooks %s\n", hooks ? "ON" : "OFF");

    trace_note("# timebase_init()\n");
    timebase_init();
    trace_note("# clock_init()\n");
    if (setjmp(fail_jmp) == 0) {
        clock_init();
    }
    trace_note("# clock_adc_set_div(200) - DIVSWEN self-clearing, then CLKRDY\n");
    if (setjmp(fail_jmp) == 0) {
        uint32_t rc = clock_adc_set_div(200u);
        trace_note("# -> %lu (%s)\n", (unsigned long)rc, clock_adc_div_error(rc));
    }
    trace_flush();
    printf("# end, %lu SFR accesses trapped\n", trace_accesses());
    return 0;
}

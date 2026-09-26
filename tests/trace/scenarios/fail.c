/*
 * fail.c (scenario, P0.5) - the clock-fail path: _CLKFInterrupt() called
 * directly, clock.c compiled unchanged.
 *
 * Sources: clock.c, timebase.c only (fail.sources). Deliberately NOT
 * diag.c and NOT capture.c:
 *
 *   - diag.c's REAL fail() never returns (it blinks the LED in a `for(;;)`
 *     loop with __delay32() between blinks, on real hardware and here
 *     alike) - linking it would make this scenario hang until the
 *     runaway watchdog kills it at TRACE_TIMEOUT_MS, which is not the
 *     behaviour under test. So diag.c is left out (no -DHAVE_DIAG here),
 *     and _CLKFInterrupt()'s call to fail(10u) resolves to stubs.c's own
 *     copy instead (longjmp back to the scenario, `F fail(10)` in the
 *     trace).
 *   - capture.c is left out too: pulling it in only to get a "real"
 *     capture_halt() would drag in adc.c/dma.c/sccp.c/led.c for a
 *     one-line call (`dma0_halt()`) that this scenario is not about.
 *     capture_halt() and console_force_up() therefore both resolve to
 *     the stubs in stubs.c (`D capture_halt()`, `C <force_up>`) - per
 *     the task's "use the real ones where linkable, stubs otherwise":
 *     neither is linkable here without pulling in unrelated modules, so
 *     both are stubbed. This is documented in the scenario table in
 *     tests/trace/README.md.
 *
 * clock.c's object file also references boot_stage (read inside
 * _CLKFInterrupt for the "[CLKF] reached boot stage" line) - stubs.c's own
 * copy supplies it, reading 0 throughout (nothing in this scenario ever
 * sets it, which is fine: deterministic, and boot_stage is not what this
 * scenario is testing).
 */
#include <setjmp.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "clock.h"

extern jmp_buf fail_jmp;

/* Declared in clock.c, not in clock.h (it is an ISR, reached only through
 * the vector table on the target): forward-declare it here so the
 * scenario can call it directly, exactly as the task asks. */
extern void __attribute__((interrupt, no_auto_psv)) _CLKFInterrupt(void);

int main(void)
{
    trace_begin("fail");

    trace_point("before _CLKFInterrupt()");
    if (setjmp(fail_jmp) == 0) {
        _CLKFInterrupt();
        trace_note("# _CLKFInterrupt() returned (unexpected: it always calls fail())\n");
    } else {
        trace_note("# fail() longjmp'd back, as expected\n");
    }
    trace_point("after _CLKFInterrupt()");

    trace_end();
    return 0;
}

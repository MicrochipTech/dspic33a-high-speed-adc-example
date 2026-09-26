/*
 * stubs.c - P0.4 register-trace harness: what timebase.c/clock.c (and
 * the scenarios after them) call outside themselves.
 *
 * Decision of 26.09.2026: console_* are stubbed straight into the trace
 * as `C` lines - cli.c is not linked into any scenario, so a change to
 * console text shows up as a trace change without needing the real UART
 * path. fail() ends the scenario step through longjmp() (it is noreturn
 * on the target too - fail() blinks forever). __delay32() is a `D` line
 * and advances the TMR1 model so a 100 ms wait costs nothing on the host.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include "recorder.h"
#include "console.h"
#include "diag.h"
#include "capture.h"

jmp_buf fail_jmp;
volatile uint32_t fail_code;
volatile uint32_t boot_stage;      /* diag.c global; _CLKFInterrupt reports it */

void console_puts(const char *s)                 { trace_note("C %s", s); }
void console_trace(const char *s)                { trace_note("C %s", s); }
void console_kv(const char *k, uint32_t v)       { trace_note("C %s: %lu\r\n", k, (unsigned long)v); }
void console_trace_kv(const char *k, uint32_t v) { trace_note("C %s: %lu\r\n", k, (unsigned long)v); }

void console_kv_hex(const char *k, uint32_t v)
{
    char b[48];
    trace_note("C %s: %s\r\n", k, trace_value(v, b, sizeof b));
}

void console_trace_kv_hex(const char *k, uint32_t v)
{
    char b[48];
    trace_note("C %s: %s\r\n", k, trace_value(v, b, sizeof b));
}

void console_flush(void)      { trace_note("C <flush>\n"); }
void console_force_up(void)   { trace_note("C <force_up>\n"); }

void capture_halt(void)       { trace_note("D capture_halt()\n"); }

void fail(uint32_t code)
{
    fail_code = code;
    trace_note("F fail(%lu)\n", (unsigned long)code);
    longjmp(fail_jmp, 1);
}

/* __delay32(): a D line, and Timer1 (TMR1, 12.5 MHz) advances by
 * cycles/16 (CPU 200 MHz), so timebase_check() sees a fixed value
 * without the scenario actually waiting. */
void __delay32(unsigned long cycles)
{
    trace_note("D __delay32(%lu)\n", (unsigned long)cycles);
    unsigned t = trace_idx("TMR1");
    hw_set(t, hw_get(t) + (uint32_t)(cycles / 16u));
}

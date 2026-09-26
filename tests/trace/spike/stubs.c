/* stubs.c - P0.3 spike: what dma.c and timebase.c call outside themselves.
 * Console output goes into the trace as C lines, with host addresses made
 * symbolic; fail() ends the scenario step through longjmp (on the target
 * it never returns either). */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include "recorder.h"
#include "console.h"
#include "diag.h"

jmp_buf fail_jmp;
volatile uint32_t fail_code;

void console_puts(const char *s)  { trace_note("C %s", s); }
void console_trace(const char *s) { trace_note("C %s", s); }

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

void fail(uint32_t code)
{
    fail_code = code;
    trace_note("F fail(%lu)\n", (unsigned long)code);
    longjmp(fail_jmp, 1);
}

void dma0_event(uint32_t status)
{
    trace_note("D dma0_event(0x%08lX)\n", (unsigned long)status);
}

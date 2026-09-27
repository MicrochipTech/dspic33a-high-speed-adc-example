/*
 * stubs.c - P0.4/P0.5 register-trace harness: what the firmware files
 * call outside themselves, when the real implementation is not linked
 * into a given scenario.
 *
 * Decision of 26.09.2026: console_* are stubbed straight into the trace
 * as `C` lines - cli.c is not linked into any scenario, so a change to
 * console text shows up as a trace change without needing the real UART
 * path. fail() ends the scenario step through longjmp() (it is noreturn
 * on the target too - fail() blinks forever). __delay32() is a `D` line
 * and advances the TMR1 model so a 100 ms wait costs nothing on the host.
 *
 * P0.5 additions and why they are guarded by HAVE_DIAG / HAVE_CAPTURE,
 * not by `__attribute__((weak))`:
 *
 *   `fail_code`, `boot_stage`, `chain_mark`, `fail()` and `capture_halt()`
 *   also have a REAL definition, in diag.c and capture.c respectively -
 *   and P0.5 introduces scenarios that link one or both (`regs` needs
 *   diag.c's real regs_dump(); `boot`, `nano`, `b2b`, `variants`, `clk`,
 *   `stream_on(_input)` need capture.c). The obvious fix, marking the
 *   stub copies `__attribute__((weak))`, does NOT work on this toolchain:
 *   measured directly (two minimal .c files, one `weak` definition, one
 *   caller) - MinGW-w64 gcc 16.1.0 / GNU ld report "undefined reference"
 *   for a plain weak function definition with no other definition and no
 *   `alias(...)` target, when every object is given to the linker
 *   directly rather than pulled from a `.a` archive. PE/COFF weak
 *   externals need more than ELF's "define it weak, the strong one wins if
 *   there is one" - not worth chasing further. Instead, every scenario
 *   that links diag.c passes `-DHAVE_DIAG` (its .cflags file) and every
 *   one that links capture.c passes `-DHAVE_CAPTURE`: this file's own
 *   copies of the five symbols below compile out exactly when the real
 *   ones will be linked instead, with an ordinary, portable `#ifndef`.
 *   `chain_mark` (diag.c's global, referenced by chaintest.c's `mark()`,
 *   `chain_all()`, ... whether or not the scenario calls those particular
 *   functions - a C link pulls in every symbol a linked .o references,
 *   not just the ones actually reached) follows HAVE_DIAG, since diag.c is
 *   its real home; `stream_on`/`stream_on_input` link chaintest.c but not
 *   diag.c, so they get this file's copy.
 *
 *   `console_early_init()`, `cli_init()`, `console_sync_baud()` and
 *   `console_regs_dump()` have no alternative real definition anywhere
 *   under test - they live in cli.c only, which decision 1 (26.09.2026)
 *   excludes from every scenario without exception - so they are plain,
 *   unconditional stubs, same as the other console_* functions below.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include "recorder.h"
#include "console.h"
#include "diag.h"
#include "capture.h"

jmp_buf fail_jmp;

#ifndef HAVE_DIAG
/* diag.c defines real, strong fail_code/boot_stage/chain_mark/fail() when
 * linked (regs.cflags: -DHAVE_DIAG). See the file header. */
volatile uint32_t fail_code;
volatile uint32_t boot_stage;      /* diag.c global; _CLKFInterrupt reports it */
volatile uint32_t chain_mark;      /* diag.c global; chaintest.c's mark() etc. */
#endif

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

/* cli.c only, never linked (decision 1, 26.09.2026): stubbed unconditionally.
 * console_early_init() would otherwise set the console pins/PPS and the FRC
 * baud generator (U2CON/U2BRG/RPOR/RPINR); cli_init() would re-set the baud
 * generator for the PLL clock and register ~26 commands with cmd_parser
 * (also excluded). Neither effect is visible in any P0.5 golden trace - see
 * the `boot`/`nano` scenario comments. */
void console_early_init(void) { trace_note("C <console_early_init, stub: cli.c not linked>\n"); }
void cli_init(void)           { trace_note("C <cli_init, stub: cli.c not linked>\n"); }
/* Only reachable through diag.c's real fail() (regs scenario links diag.c
 * but never calls fail()), so it never actually runs in any golden trace;
 * kept as a plain no-op purely to satisfy the linker. */
void console_sync_baud(void)  { }
/* diag.c's real regs_dump() (regs scenario) calls this unconditionally;
 * cli.c's real one prints IEC3/IFS3/IPC12/U2CON/U2STAT/U2BRG/RPCON - none
 * of it reachable without cli.c, so this is what "regs" shows instead. */
void console_regs_dump(void)  { trace_note("C [regs] uart (stub: cli.c not linked, no UART registers here)\n"); }

#ifndef HAVE_CAPTURE
/* capture.c defines a real, strong capture_halt() (dma0_halt()) when
 * linked (every scenario whose .cflags sets -DHAVE_CAPTURE). */
void capture_halt(void) { trace_note("D capture_halt()\n"); }
#endif

#ifndef HAVE_CHAINTEST
/* adc.c's _AD5CH0Interrupt calls this (adc.h) whenever adc.c is linked -
 * every P0.5 scenario that touches the ADC does, whether or not it uses
 * the chain test's low-rate counting path. chaintest.c is the only real
 * implementation (its chain test evaluates the result); scenarios that
 * link it (stream_on, stream_on_input: -DHAVE_CHAINTEST) get that one
 * instead - never both, or this would be a duplicate definition. */
void adc_ch0_event(uint16_t result) { trace_note("D adc_ch0_event(%u)\n", (unsigned)result); }
#endif

#ifndef HAVE_DIAG
void fail(uint32_t code)
{
    fail_code = code;
    trace_note("F fail(%lu)\n", (unsigned long)code);
    longjmp(fail_jmp, 1);
}

/* diag.c's printing register visitor (port/regs.h, P4.8): capture.c's
 * capture_variant_regs() and chaintest.c's recipe dump pass it to the
 * drivers' xxx_regs_visit(). The same three console calls as diag.c's
 * reg_print(), so a scenario without diag.c records the same `C` lines
 * the driver's former xxx_regs_dump() produced. */
void reg_print(const char *name, uint32_t v, reg_fmt_t fmt)
{
    switch (fmt) {
    case REG_HEX:   console_kv_hex(name, v); break;
    case REG_DEC:   console_kv(name, v);     break;
    case REG_TITLE:
    default:        console_puts(name);      break;
    }
}
#endif

/* __delay32(): a D line, and Timer1 (TMR1, 12.5 MHz) advances by
 * cycles/16 (CPU 200 MHz), so timebase_check() sees a fixed value
 * without the scenario actually waiting. */
void __delay32(unsigned long cycles)
{
    trace_note("D __delay32(%lu)\n", (unsigned long)cycles);
    unsigned t = trace_idx("TMR1");
    hw_set(t, hw_get(t) + (uint32_t)(cycles / 16u));
}

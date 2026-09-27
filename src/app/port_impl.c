/*
 * port_impl.c - this project's implementation of the port layer
 * (src/port/log.h, src/port/panic.h; V2 of docs/REFACTORING-PROPOSAL.md,
 * P4.1 of docs/IMPLEMENTATION-PLAN.md, 27.09.2026)
 *
 * The drivers under src/drivers/ print and stop only through port_*; this
 * file is where the example connects them to its own console (cli.c) and
 * to fail() (diag.c). It lives in app/ because that choice - a UART
 * console, a blink code - is the application's, not the driver's.
 *
 * Trace harness (tests/trace): a scenario that links a driver using
 * port_* lists this file in its .sources as well, and the harness's
 * console_* and fail() stubs (tests/trace/harness/stubs.c) do the rest -
 * the text lands in the trace as `C` lines and a panic as an `F` line
 * exactly as when the driver called console_* and fail() directly. No
 * port stubs of its own, so there is one mapping, not two that could
 * drift apart.
 */

#include "log.h"
#include "panic.h"
#include "console.h"
#include "diag.h"

void port_log(const char *s)
{
    console_puts(s);
}

void port_log_kv(const char *key, uint32_t v, bool hex)
{
    if (hex) {
        console_kv_hex(key, v);
    } else {
        console_kv(key, v);
    }
}

void port_panic(uint32_t code)
{
    fail(code);
    /* fail() blinks the code forever (diag.c) but diag.h does not declare
     * it noreturn; without this loop the compiler reports a noreturn
     * function that returns. Never reached. */
    for (;;) { }
}

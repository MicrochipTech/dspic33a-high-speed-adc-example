/*
 * panic.h - the port layer's abort (V2 of docs/REFACTORING-PROPOSAL.md)
 *
 * A driver under src/drivers/ that hits a state it cannot go on from
 * calls port_panic() with its stop code and nothing else - not fail()
 * (diag.h), which is this project's implementation of it. The codes stay
 * the ones fail()'s table in diag.c lists (and docs/TROUBLESHOOTING.md),
 * so LED0's blink count and the "[FAIL] code" line do not change with the
 * indirection. src/app/port_impl.c maps it to fail(); in the trace
 * harness the same file links against the harness's fail() stub, which
 * ends the scenario step with a longjmp() as before.
 */
#ifndef PORT_PANIC_H
#define PORT_PANIC_H

#include <stdint.h>

/* Stop for good with a code from fail()'s table; never returns. */
void port_panic(uint32_t code) __attribute__((noreturn));

#endif /* PORT_PANIC_H */

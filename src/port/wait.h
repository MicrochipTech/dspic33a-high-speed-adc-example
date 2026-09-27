/*
 * wait.h - the port layer's bounded hardware wait (V2 of
 * docs/REFACTORING-PROPOSAL.md, P4.6a of docs/IMPLEMENTATION-PLAN.md)
 *
 * Every driver under src/drivers/ that spins on a hardware flag (a PLL
 * lock, a clock switch, ADRDY) bounds the spin with PORT_WAIT_WHILE()
 * and stops through port_panic() when the bound is reached - not with
 * diag.h's WAIT_WHILE(), which is this project's original of it and
 * calls fail() directly. The values and the two variants are diag.h's,
 * unchanged, so that a wait behaves exactly as it did: 2 000 000 loop
 * iterations on silicon, and in the simulator build no loop at all - the
 * MPLAB X simulator has no PLL, no ADC and no DMA, so every one of these
 * conditions would time out and end the run before anything of interest
 * had executed; the condition is still evaluated once, so the register
 * read still happens and the trace still sees it (the same pattern MCC's
 * clock.c uses). PORT_WAIT_LIMIT is the bound on its own, for a driver
 * that counts a loop down itself and reports the result instead of
 * stopping (adc_reinit()). The simulator value, 20 000, is diag.h's too
 * (the simulator runs at ~1/80 real time).
 *
 * The stop codes stay the ones fail()'s table in diag.c lists, so the
 * LED blink count and docs/TROUBLESHOOTING.md do not change: 1..4 for
 * clock.c's PLL and generator switches, 5 for adc.c's ADRDY.
 */
#ifndef PORT_WAIT_H
#define PORT_WAIT_H

#include <stdint.h>
#include "panic.h"

#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define PORT_WAIT_LIMIT        20000u     /* simulator runs at ~1/80 real time */
#define PORT_WAIT_WHILE(cond, code)  do { (void)(cond); } while (0)
#else
#define PORT_WAIT_LIMIT        2000000u
#define PORT_WAIT_WHILE(cond, code)                         \
    do {                                                    \
        uint32_t n_ = PORT_WAIT_LIMIT;                      \
        while (cond) {                                      \
            if (--n_ == 0u) { port_panic(code); }           \
        }                                                   \
    } while (0)
#endif

#endif /* PORT_WAIT_H */

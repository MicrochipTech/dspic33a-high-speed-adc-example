/*
 * hwmodel.h - P0.4 register-trace harness: the "hardware model" that
 * answers polling loops so approach (a) (no per-access read hook, see
 * tests/trace/README.md) still lets a driver's busy-wait terminate.
 *
 * A rule is level-triggered and idempotent: while the model runs, it
 * repeatedly forces `clear_mask` bits of `reg` to 0 and `set_mask` bits
 * to 1, over and over, regardless of what the register held before. That
 * is what makes the outcome deterministic despite running on a real OS
 * thread with no fixed schedule relative to the driver thread: applying
 * a rule before, during or after the driver sets the bit it waits on
 * gives the same final state (0 in the switch-enable bit, 1 in the ready
 * bit) - there is no ordering for the two threads' races to disagree
 * about. What is NOT deterministic, and does not need to be, is exactly
 * how many times the driver's loop re-read the bit before it changed;
 * approach (a) cannot see that anyway (it only diffs at trace points).
 */
#ifndef HWMODEL_H
#define HWMODEL_H

#include <stdint.h>

typedef struct {
    const char *reg;         /* SFR name, e.g. "PLL1CON"                */
    uint32_t    clear_mask;  /* bits forced to 0 every iteration        */
    uint32_t    set_mask;    /* bits forced to 1 every iteration        */
} hwmodel_rule_t;

/* Starts a background thread that applies `rules[0..n_rules)` in a tight
 * loop (via hw_get()/hw_set(), so it never appears as a driver write),
 * plus, if tmr1_step != 0, advances TMR1 by tmr1_step every iteration.
 * `rules` must stay valid until hwmodel_stop(). Call after trace_begin(). */
void hwmodel_start(const hwmodel_rule_t *rules, unsigned n_rules, uint32_t tmr1_step);

/* Stops and joins the thread. Call before trace_end(). A no-op if
 * hwmodel_start() was never called. */
void hwmodel_stop(void);

#endif /* HWMODEL_H */

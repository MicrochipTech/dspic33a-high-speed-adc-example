/*
 * hwmodel.c - P0.4 register-trace harness: the hardware model thread
 * (hwmodel.h). Windows-specific (CreateThread), like the rest of the
 * harness (tests/trace/README.md).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "recorder.h"
#include "hwmodel.h"

#define HWMODEL_MAX_RULES 16u

static hwmodel_rule_t rules_copy[HWMODEL_MAX_RULES];
static unsigned       rule_idx[HWMODEL_MAX_RULES];
static unsigned       n_rules_g;
static uint32_t       tmr1_step_g;
static unsigned       tmr1_idx_g;
static volatile LONG  stop_flag;
static HANDLE         thread_h;
static HANDLE         started_event;
static DWORD_PTR      main_affinity_restore;   /* 0: nothing to restore */
/* Bumped once per loop iteration, rules or not - hwmodel_start()'s own
 * proof that the model is not merely alive (started_event) but actually
 * completing fresh iterations, right before it hands control back. See
 * the long comment in hwmodel_start() for why this exists. */
static volatile LONG  heartbeat;

static DWORD WINAPI model_thread(LPVOID unused)
{
    (void)unused;
    SetEvent(started_event);
    while (!stop_flag) {
        for (unsigned i = 0; i < n_rules_g; i++) {
            uint32_t mask = rules_copy[i].clear_mask | rules_copy[i].set_mask;
            if (mask == 0u) { continue; }
            /* Touch only the bits this rule owns (hw_set_masked) - never
             * the whole register, or a driver write to some OTHER field
             * of the same SFR that trace_flush() has not diffed yet would
             * be silently folded into shadow[] and vanish from the trace
             * (tests/trace/README.md, hw_set_masked() in recorder.h). */
            if ((hw_get(rule_idx[i]) & mask) != rules_copy[i].set_mask) {
                hw_set_masked(rule_idx[i], mask, rules_copy[i].set_mask);
            }
        }
        if (tmr1_step_g != 0u) {
            hw_set(tmr1_idx_g, hw_get(tmr1_idx_g) + tmr1_step_g);
        }
        InterlockedIncrement(&heartbeat);
    }
    return 0;
}

void hwmodel_start(const hwmodel_rule_t *rules, unsigned n_rules, uint32_t tmr1_step)
{
    if (n_rules > HWMODEL_MAX_RULES) { n_rules = HWMODEL_MAX_RULES; }
    n_rules_g = n_rules;
    for (unsigned i = 0; i < n_rules; i++) {
        rules_copy[i] = rules[i];
        rule_idx[i] = trace_idx(rules[i].reg);
    }
    tmr1_step_g = tmr1_step;
    if (tmr1_step != 0u) { tmr1_idx_g = trace_idx("TMR1"); }
    stop_flag = 0;
    /* P0.5: a driver wait bounded to DIVSW_WAIT_LIMIT (100 000 iterations,
     * clock.c: clock_dac_on(), clock_trig_on(), clock_adc_set_pll/
     * _set_rate/_set_div's per-step waits) can run its course in well
     * under the time CreateThread() itself takes to schedule a brand new
     * thread's first instruction - measured directly (a throwaway repro:
     * tests/trace/scenarios/dac.c's clock_dac_on() call with the model
     * started right beforehand), 50-95% of runs saw the model never
     * intervene at all before the wait gave up, on a 14-logical-core host,
     * regardless of the model thread's priority (a higher priority made it
     * WORSE - Sleep(0) only yields to threads of EQUAL OR HIGHER priority,
     * so a HIGHER-priority model thread stopped yielding to the driver
     * thread at all). clock_init()'s own WAIT_WHILE loops (WAIT_LIMIT,
     * 2 000 000 iterations - 20x longer) were not observed to fail the
     * same way, which is why the P0.4 spike's 15/5 determinism runs
     * (clock_init() only) never surfaced this: apparently that wait is
     * long enough to outlast CreateThread()'s own latency, and the shorter
     * one usually is not.
     *
     * Fix: block here until the new thread has actually started running -
     * proven with an event it sets as its first statement, not assumed -
     * so CreateThread()'s latency is paid for HERE, before the caller goes
     * on to make the driver call whose wait this model must answer, not
     * raced against it. Brought the failure rate from 50-95/100 down to
     * about 1/100 (see the next comment for the residual and how it is
     * actually closed - the scenario-level retry, not this fix alone). */
    heartbeat = 0;
    started_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    thread_h = CreateThread(NULL, 0, model_thread, NULL, 0, NULL);
    /* Pin the model onto its own core, away from whatever core the main
     * (driver) thread is on, so the two never compete for the same core -
     * one whole class of scheduling noise (something else briefly using
     * the model's core) removed outright rather than raced against. Best
     * effort: if the host has only one logical core, both masks below
     * collapse to the same bit and this is a no-op, exactly like running
     * without it. */
    {
        DWORD_PTR proc_mask = 0, sys_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) && (proc_mask != 0)) {
            DWORD_PTR hi = 1u;
            for (DWORD_PTR b = proc_mask; b != 0; b &= (b - 1)) { hi = b; }  /* highest set bit */
            DWORD_PTR lo = proc_mask & (~proc_mask + 1u);                    /* lowest set bit  */
            SetThreadAffinityMask(thread_h, hi);
            main_affinity_restore = SetThreadAffinityMask(GetCurrentThread(), lo);
        }
    }
    WaitForSingleObject(started_event, INFINITE);
    /* The event only proves the new thread has executed ITS FIRST
     * STATEMENT - not that it is done with whatever one-time warm-up cost
     * a brand new thread has (first page faults for its stack, first
     * scheduling on whichever core it lands on, ...) and settled into
     * really, repeatedly running its loop. Measured directly
     * (tests/trace/scenarios/dac.c's clock_dac_on(), the shortest wait in
     * the firmware - DIVSW_WAIT_LIMIT, 100 000 iterations, on a 14-logical-
     * core host): the event alone still left about 1% of runs where the
     * model never intervened in time (0, 2, 1 failures per 100 over three
     * rounds); a fixed busy-wait here (a few milliseconds, no Sleep/Wait
     * call - a `Sleep(1)` made failures MUCH worse, 30-46/150, not
     * explained and not worth chasing) and pinning the model onto its own
     * core (below) each narrowed it a little but never reliably closed it
     * (0-2 per 100/200, repeatedly). Waiting for firm PROOF of live
     * throughput instead - the model's own heartbeat, bumped once per loop
     * iteration, has advanced by a healthy margin since the ready event
     * fired - measured the same residual ~1% (1-2 per 100-200). This is
     * not a fixed guess at "long enough" and it does measurably help
     * (every variant above it was tried and kept, on the reasoning that
     * each removes one real, separate source of delay), but it does not
     * fully close the gap on this host, and further chasing it here has
     * not been worth it: every call this model answers is retried at the
     * SCENARIO level instead (silently, no trace_point() between attempts
     * - see e.g. tests/trace/scenarios/dac.c), which turns this harness's
     * own residual ~1% single-attempt failure into a practically-zero
     * chance of a golden trace ever being affected by it (~1%^5 over five
     * attempts), which is what actually makes "record twice, compare"
     * reproducible - not a claim that the race below is fully solved.
     * Bounded by wall clock (GetTickCount64) only so a genuinely broken
     * model thread cannot hang the process here; a scenario relying on a
     * model that never became responsive would still be caught by
     * trace_begin()'s runaway watchdog once it starts waiting on the
     * driver's own bit. */
#define HWMODEL_HEARTBEAT_MARGIN   2000L
#define HWMODEL_WARMUP_BOUND_MS    2000u
    {
        const ULONGLONG t0 = GetTickCount64();
        const LONG start_hb = heartbeat;
        while (((heartbeat - start_hb) < HWMODEL_HEARTBEAT_MARGIN) &&
               ((GetTickCount64() - t0) < HWMODEL_WARMUP_BOUND_MS)) { }
    }
}

void hwmodel_stop(void)
{
    if (thread_h != NULL) {
        InterlockedExchange(&stop_flag, 1);
        WaitForSingleObject(thread_h, INFINITE);
        CloseHandle(thread_h);
        thread_h = NULL;
        CloseHandle(started_event);
        started_event = NULL;
        if (main_affinity_restore != 0) {
            SetThreadAffinityMask(GetCurrentThread(), main_affinity_restore);
            main_affinity_restore = 0;
        }
    }
}

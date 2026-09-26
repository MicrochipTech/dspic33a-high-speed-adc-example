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

static DWORD WINAPI model_thread(LPVOID unused)
{
    (void)unused;
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
        Sleep(0);   /* yield; do not spin at 100% longer than necessary */
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
    thread_h = CreateThread(NULL, 0, model_thread, NULL, 0, NULL);
}

void hwmodel_stop(void)
{
    if (thread_h != NULL) {
        InterlockedExchange(&stop_flag, 1);
        WaitForSingleObject(thread_h, INFINITE);
        CloseHandle(thread_h);
        thread_h = NULL;
    }
}

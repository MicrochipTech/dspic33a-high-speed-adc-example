/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

/*
 * hwmodel.c - P0.5b register-trace harness: the page-guarded read hook
 * (hwmodel.h). Windows-specific (VirtualProtect, AddVectoredExceptionHandler),
 * like the rest of the harness (tests/trace/README.md).
 *
 * Replaces P0.4/P0.5's background "hardware model" thread. That thread
 * was correct in the limit (every rule idempotent and level-triggered,
 * tests/trace/README.md's original hwmodel.c header) but raced a short
 * enough driver wait on this host about 1 time in 100-200
 * (DIVSW_WAIT_LIMIT, 100000 iterations) - CreateThread()'s own latency
 * could outlast the whole wait. P0.5 lived with that by retrying every
 * affected call up to 5x (or, for variants.c, 3x unconditionally) at the
 * SCENARIO level. The user's decision of 27.09.2026 ("the hybrid",
 * tests/trace/README.md) replaces the thread outright: the same rule
 * table now answers a driver's read of the polled register synchronously,
 * on the driver's own thread, via a Windows vectored exception handler -
 * adapted from the P0.3 spike's page-guarded recorder
 * (tests/trace/spike/recorder.c), but guarding only the page(s) that
 * contain a scenario's own polled registers (plus TMR1, if used) instead
 * of the spike's whole sfr_mem[] array, and applying a rule only on a
 * READ of the exact register it names - a write, or an access to any
 * other register that happens to share the same guarded page, just
 * passes through unmodified (single-stepped and re-guarded, nothing
 * else). Approach (a)'s snapshot diff (recorder.c) is completely
 * unaffected and remains the recorded trace; this file never prints
 * anything.
 *
 * Why this closes the race rather than narrowing it further: there is no
 * second thread any more, so there is nothing left for the driver thread
 * to race against. The instant the driver's code reads a polled
 * register, the hook's rule has already been applied to it (clear the
 * self-clearing switch-enable bits, set the hardware-set ready bits,
 * advance TMR1 by the scenario's step) - the very first read always sees
 * the answer, deterministically, every time. A `WAIT_WHILE` loop that
 * used to need an unknown, scheduling-dependent number of iterations
 * before the model thread got around to clearing the bit now exits after
 * exactly one.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef STRICT   /* windows.h #defines STRICT 1; the device header has a
                 * same-named bit field (e.g. FICD's STRICT) - same fix
                 * as recorder.c. */
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"

#define HWMODEL_MAX_RULES 16u
#define HWMODEL_MAX_PAGES 4u    /* sfr_mem[] is exactly 4 pages, aligned  */

typedef struct { unsigned idx; uint32_t clear_mask; uint32_t set_mask; } guard_rule_t;

static guard_rule_t g_rules[HWMODEL_MAX_RULES];
static unsigned     g_n_rules;
static unsigned     g_tmr1_idx = ~0u;     /* ~0u: no TMR1 rule armed      */
static uint32_t     g_tmr1_step;

static uintptr_t    g_pages[HWMODEL_MAX_PAGES];
static unsigned     g_n_pages;
static DWORD        g_page_size;
static PVOID        g_veh;
static uintptr_t    g_reguard_page;       /* page to re-protect after the
                                            * single step; 0 = none due   */

static uintptr_t page_of(uintptr_t addr)
{
    return addr & ~((uintptr_t)g_page_size - 1u);
}

static const guard_rule_t *rule_for(unsigned idx)
{
    for (unsigned i = 0; i < g_n_rules; i++) {
        if (g_rules[i].idx == idx) { return &g_rules[i]; }
    }
    return NULL;
}

static void add_page(uintptr_t addr)
{
    uintptr_t page = page_of(addr);
    for (unsigned i = 0; i < g_n_pages; i++) {
        if (g_pages[i] == page) { return; }
    }
    if (g_n_pages < HWMODEL_MAX_PAGES) { g_pages[g_n_pages++] = page; }
}

/*
 * sfr_hook() - the vectored exception handler.
 *
 * EXCEPTION_ACCESS_VIOLATION on one of our guarded pages: if it is a READ
 * of the exact register a rule names, apply the rule (or, for TMR1,
 * advance it) to sfr_mem[]/shadow[] together (hw_set_masked()/hw_set(),
 * recorder.c - so the change is a hardware change, not a driver write,
 * and never appears in the trace) BEFORE letting the faulting instruction
 * run - unprotect the page, arm the trap flag, resume. Any other access
 * on a guarded page (a write, or a read of some other register that
 * happens to share the page) is passed straight through the same way,
 * with no rule applied - it is not ours to touch.
 *
 * EXCEPTION_SINGLE_STEP right after: the one instruction we let through
 * has now executed exactly once; re-guard the page and clear the trap
 * flag. Only one instruction is ever single-stepped at a time in this
 * harness (nothing here re-enters the handler from within itself), so a
 * single pending-page variable is enough - no stack of pending faults.
 */
static LONG CALLBACK sfr_hook(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;

    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        if (g_reguard_page != 0u) {
            DWORD old;
            VirtualProtect((void *)g_reguard_page, g_page_size, PAGE_NOACCESS, &old);
            g_reguard_page = 0u;
            ep->ContextRecord->EFlags &= ~0x100u;   /* clear TF - or every
                                                      * instruction from here
                                                      * on traps, not just
                                                      * the guarded one */
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uintptr_t addr = (uintptr_t)er->ExceptionInformation[1];
    uintptr_t page = page_of(addr);
    unsigned  pi;
    for (pi = 0; pi < g_n_pages; pi++) {
        if (g_pages[pi] == page) { break; }
    }
    if (pi == g_n_pages) { return EXCEPTION_CONTINUE_SEARCH; }   /* not ours */

    /* Unprotect BEFORE touching sfr_mem[] for the rule below - hw_get()/
     * hw_set_masked() read and write sfr_mem[idx] directly, and idx's page
     * is still PAGE_NOACCESS at this point. Applying the rule first (as
     * an earlier version of this handler did) makes that write itself
     * fault, re-entering this handler for an access it did not expect
     * (g_reguard_page is not stack-based) - observed as an infinite
     * re-fault loop at the same instruction, killed only by the runaway
     * watchdog. Unprotecting first means the rule's own access is just an
     * ordinary read/write of now-writable memory. */
    DWORD old;
    VirtualProtect((void *)page, g_page_size, PAGE_READWRITE, &old);

    int is_write = (er->ExceptionInformation[0] == 1);
    uintptr_t base = (uintptr_t)&sfr_mem[0];
    if (!is_write && (addr >= base) && (addr < base + ((uintptr_t)SFR_COUNT * 4u))) {
        unsigned idx = (unsigned)((addr - base) / 4u);
        const guard_rule_t *r = rule_for(idx);
        if (r != NULL) {
            hw_set_masked(idx, r->clear_mask | r->set_mask, r->set_mask);
        } else if (idx == g_tmr1_idx) {
            hw_set(idx, hw_get(idx) + g_tmr1_step);
        }
    }

    g_reguard_page = page;
    ep->ContextRecord->EFlags |= 0x100u;         /* TF: trap after this insn */
    return EXCEPTION_CONTINUE_EXECUTION;
}

void hwmodel_start(const hwmodel_rule_t *rules, unsigned n_rules, uint32_t tmr1_step)
{
    if (n_rules > HWMODEL_MAX_RULES) { n_rules = HWMODEL_MAX_RULES; }
    if (g_page_size == 0u) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_page_size = si.dwPageSize;
    }

    g_n_rules = n_rules;
    g_n_pages = 0;
    g_reguard_page = 0u;
    for (unsigned i = 0; i < n_rules; i++) {
        unsigned idx = trace_idx(rules[i].reg);
        g_rules[i].idx        = idx;
        g_rules[i].clear_mask = rules[i].clear_mask;
        g_rules[i].set_mask   = rules[i].set_mask;
        add_page((uintptr_t)&sfr_mem[idx]);
    }

    g_tmr1_step = tmr1_step;
    if (tmr1_step != 0u) {
        g_tmr1_idx = trace_idx("TMR1");
        add_page((uintptr_t)&sfr_mem[g_tmr1_idx]);
    } else {
        g_tmr1_idx = ~0u;
    }

    g_veh = AddVectoredExceptionHandler(1, sfr_hook);
    for (unsigned i = 0; i < g_n_pages; i++) {
        DWORD old;
        VirtualProtect((void *)g_pages[i], g_page_size, PAGE_NOACCESS, &old);
    }
}

void hwmodel_stop(void)
{
    for (unsigned i = 0; i < g_n_pages; i++) {
        DWORD old;
        VirtualProtect((void *)g_pages[i], g_page_size, PAGE_READWRITE, &old);
    }
    g_n_pages = 0;
    if (g_veh != NULL) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = NULL;
    }
    g_reguard_page = 0u;
}

/*
 * hwmodel_pause()/hwmodel_resume() - bracket recorder.c's own trace_flush()
 * diff loop, which walks every one of SFR_COUNT sfr_mem[] entries in
 * address order to compare it against shadow[] (recorder.c). That scan
 * itself reads whichever polled registers are currently guarded - not a
 * driver's deliberate poll, just recorder.c's own bookkeeping - and
 * without this, each such incidental read fired the rule exactly as a
 * real poll would (found by testing: `clock`'s trace gained a spurious
 * `W OSCCTRL 0x0 -> 0xC000` between hwmodel_start() and the first
 * trace_point(), from the diff loop's own read of OSCCTRL racing its own
 * read of shadow[] against the very mutation it triggered). Pausing
 * un-guards every page hwmodel_start() guarded for the duration of the
 * scan (single-threaded - nothing else runs while it does), so
 * trace_flush() sees the true, unmutated values, exactly like a scenario
 * that never called hwmodel_start() at all; resuming re-guards the same
 * pages. A no-op when hwmodel_start() was never called (g_n_pages == 0),
 * so recorder.c can call these unconditionally around every diff. */
void hwmodel_pause(void)
{
    for (unsigned i = 0; i < g_n_pages; i++) {
        DWORD old;
        VirtualProtect((void *)g_pages[i], g_page_size, PAGE_READWRITE, &old);
    }
}

void hwmodel_resume(void)
{
    for (unsigned i = 0; i < g_n_pages; i++) {
        DWORD old;
        VirtualProtect((void *)g_pages[i], g_page_size, PAGE_NOACCESS, &old);
    }
}

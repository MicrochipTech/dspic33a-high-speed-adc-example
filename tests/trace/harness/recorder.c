/*
 * recorder.c - register-trace recorder, approach (a): snapshot diff in
 * plain C (tests/trace/README.md, decision of 26.09.2026, evolved from
 * the P0.3 spike's tests/trace/spike/recorder.c).
 *
 * Trace line format (one event per line), unchanged from the spike:
 *   W NAME old -> new     a write: the net change since the previous
 *                         trace_point()/trace_note(), found by diffing
 *                         every SFR against a shadow copy, in address
 *                         order (determinism: two scenarios that write
 *                         the same registers to the same values always
 *                         list them in the same order, regardless of
 *                         the order the driver wrote them in - which is
 *                         exactly what approach (a) cannot see, and the
 *                         accepted consequence per the decision).
 *   C text                console output of the driver (stub, no cli.c)
 *   D text                other stubs (__delay32, capture_halt, ...)
 *   F fail(n)             fail() - longjmp back to the scenario
 *   # text                the scenario's own comments / trace_point()
 * Values that are host addresses of an SFR or of a registered RAM region
 * are printed symbolically (&AD5CH0RES(0x000DA4), &buf+0x0), because the
 * host address is neither the target's value nor deterministic.
 *
 * P0.5b (27.09.2026, tests/trace/README.md, "the hybrid"): polling loops
 * are now answered by hwmodel.c's page-guarded read hook (a vectored
 * exception handler on this same thread, armed with VirtualProtect over
 * just the SFR pages a scenario's rules name), not by a second OS thread.
 * There is therefore only ever one thread touching sfr_mem[]/shadow[] -
 * the hook fires synchronously, on the very thread whose read faulted -
 * so the CRITICAL_SECTION P0.4/P0.5 needed to serialise a background
 * model thread against trace_flush()'s diff is gone; hw_get()/
 * hw_set_masked() are plain, unlocked accesses now. sfr_mem[] is
 * page-aligned (below) so hwmodel.c's VirtualProtect calls land on exact
 * page boundaries and never guard a byte of any other global.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef STRICT   /* windows.h #defines STRICT 1; the device header has a
                 * same-named bit field (e.g. FICD's STRICT) */
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"

_Static_assert(SFR_COUNT <= SFR_MEM_WORDS, "raise SFR_MEM_WORDS in sfr_host.h");

/* Page-aligned: SFR_MEM_WORDS (4096) * 4 bytes = 16384 bytes = exactly
 * 4 * 4096-byte pages, so hwmodel.c's VirtualProtect(page-of(&sfr_mem[idx]))
 * always covers whole pages of sfr_mem[] alone - never a byte of shadow[]
 * or any other global (tests/trace/README.md, "the hybrid"). */
volatile uint32_t sfr_mem[SFR_MEM_WORDS] __attribute__((aligned(4096)));
static uint32_t shadow[SFR_MEM_WORDS];

static struct { uintptr_t lo; size_t n; const char *name; } regions[8];
static unsigned n_regions;

/* ---- runaway guard ------------------------------------------------------
 * Every WAIT_WHILE() in the drivers is bounded (diag.h, WAIT_LIMIT loop
 * iterations, not wall-clock time), so a bit the hardware model answers
 * always lets the loop finish - but a scenario that calls into code with
 * a software wait the model does NOT cover (e.g. an ISR-set RAM flag,
 * decision of 26.09.2026: out of scope, such entry points are left out of
 * scenarios) could still spin for a long time or, in principle, forever.
 * A wall-clock watchdog thread aborts the process with a clear message
 * instead of leaving trace.bat hanging with no explanation. Default
 * 5000 ms; TRACE_TIMEOUT_MS overrides it (the 5 host tests/host runs in
 * the determinism check all finish in well under 1 s). */
static HANDLE watchdog_thread;
static HANDLE watchdog_stop;

static DWORD WINAPI watchdog_proc(LPVOID arg)
{
    DWORD ms = (DWORD)(uintptr_t)arg;
    if (WaitForSingleObject(watchdog_stop, ms) == WAIT_TIMEOUT) {
        fflush(stdout);
        fprintf(stderr,
                "trace: scenario exceeded %lu ms wall clock - aborting.\n"
                "       A polling loop the hardware model does not answer?\n"
                "       (tests/trace/README.md, \"runaway guard\")\n",
                (unsigned long)ms);
        fflush(stderr);
        _exit(3);
    }
    return 0;
}


/* ---- formatting -------------------------------------------------------- */
const char *trace_value(uint32_t v, char *buf, size_t len)
{
    const uint32_t base = (uint32_t)(uintptr_t)&sfr_mem[0];
    if (((v - base) < SFR_COUNT * 4u) && (((v - base) % 4u) == 0u)) {
        unsigned i = (v - base) / 4u;
        snprintf(buf, len, "&%s(0x%06lX)", sfr_info[i].name, (unsigned long)sfr_info[i].addr);
        return buf;
    }
    for (unsigned r = 0; r < n_regions; r++) {
        uint32_t lo = (uint32_t)regions[r].lo;
        if ((v - lo) <= regions[r].n) {      /* <= : "last byte + 1" too */
            snprintf(buf, len, "&%s+0x%lX", regions[r].name, (unsigned long)(v - lo));
            return buf;
        }
    }
    snprintf(buf, len, "0x%08lX", (unsigned long)v);
    return buf;
}

static void log_write(unsigned i, uint32_t old, uint32_t now)
{
    char a[48], b[48];
    printf("W %-12s %s -> %s\n", sfr_info[i].name,
           trace_value(old, a, sizeof a), trace_value(now, b, sizeof b));
}

/* The diff: every SFR in address order (idx is already rank-of-address,
 * tools/gen_fake_sfr.py), so the output order is deterministic and does
 * not depend on which order the driver happened to write them in. */
static void trace_flush(void)
{
    /* hwmodel_pause(): this scan itself reads every polled register, which
     * would otherwise fire its rule as if a driver had polled it (see
     * hwmodel.c's own comment on hwmodel_pause()/hwmodel_resume()). A
     * no-op when hwmodel_start() was never called. */
    hwmodel_pause();
    for (unsigned i = 0; i < SFR_COUNT; i++) {
        if (sfr_mem[i] != shadow[i]) {
            log_write(i, shadow[i], sfr_mem[i]);
            shadow[i] = sfr_mem[i];
        }
    }
    hwmodel_resume();
}

/* ---- harness API --------------------------------------------------------*/
void trace_begin(const char *scenario)
{
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    /* Reset values (P0.9, 27.09.2026): every SFR starts at the device's
     * reset value, sfr_reset[] from the pack's ATDF `initval` (generated
     * into sfr_table.c by tools/gen_fake_sfr.py; an SFR the ATDF has no
     * register for is 0 there). Until P0.9 every SFR started at 0, which
     * left a field no driver writes (AD3CON.RPTCNT = 18 at reset) wrong
     * in the trace's absolute values - P0.8's one substantive finding.
     * The shadow starts equal, so the preset itself is never a W line:
     * the trace shows what the DRIVER changed, starting from reset. Words
     * beyond SFR_COUNT (the array is page-rounded) stay 0. */
    memset((void *)sfr_mem, 0, sizeof sfr_mem);
    memcpy((void *)sfr_mem, sfr_reset, SFR_COUNT * sizeof sfr_mem[0]);
    memcpy(shadow, (const void *)sfr_mem, sizeof shadow);
    n_regions = 0;

    const char *e = getenv("TRACE_TIMEOUT_MS");
    DWORD ms = e ? (DWORD)strtoul(e, NULL, 10) : 5000u;
    watchdog_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    watchdog_thread = CreateThread(NULL, 0, watchdog_proc, (LPVOID)(uintptr_t)ms, 0, NULL);

    printf("# scenario %s\n", scenario);
    printf("# layout check: %d fields differ from the pack's masks\n", sfr_layout_check());
}

void trace_point(const char *label)
{
    trace_flush();
    printf("# %s\n", label);
}

void trace_note(const char *fmt, ...)
{
    va_list ap;
    trace_flush();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void trace_end(void)
{
    trace_flush();
    if (watchdog_thread != NULL) {
        SetEvent(watchdog_stop);
        WaitForSingleObject(watchdog_thread, INFINITE);
        CloseHandle(watchdog_thread);
        CloseHandle(watchdog_stop);
        watchdog_thread = NULL;
    }
    fflush(stdout);
}

unsigned trace_idx(const char *name)
{
    for (unsigned i = 0; i < SFR_COUNT; i++) {
        if (strcmp(sfr_info[i].name, name) == 0) { return i; }
    }
    fprintf(stderr, "trace_idx: no SFR %s\n", name);
    exit(2);
}

void trace_region(const volatile void *p, size_t n, const char *name)
{
    regions[n_regions].lo = (uintptr_t)p;
    regions[n_regions].n = n;
    regions[n_regions].name = name;
    n_regions++;
}

uint32_t hw_get(unsigned idx)
{
    return sfr_mem[idx];
}

void hw_set_masked(unsigned idx, uint32_t mask, uint32_t value)
{
    uint32_t nv = (sfr_mem[idx] & ~mask) | (value & mask);
    sfr_mem[idx] = nv;                 /* hardware change: not a driver write */
    shadow[idx] = (shadow[idx] & ~mask) | (value & mask);  /* ... so only the
                                        * hardware-owned bits are hidden from
                                        * trace_flush(); any other bits stay
                                        * whatever the driver last wrote and
                                        * are still diffed normally */
}

void hw_set(unsigned idx, uint32_t v)
{
    hw_set_masked(idx, ~0u, v);
}

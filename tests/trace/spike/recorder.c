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
 * recorder.c - P0.3 spike: records what the drivers write to the SFRs.
 *
 * Trace line format (one event per line):
 *   W NAME old -> new     a write (mode 3: every write, in order, also
 *                         one that leaves the value unchanged; modes 1/2:
 *                         a changed value found by the diff)
 *   R NAME [xN]           a read (mode 3 with TRACE_READS=1 only),
 *                         consecutive reads of one register collapsed
 *   C text                console output of the driver (stub)
 *   D text                __delay32() and other stubs
 *   # text                the scenario's own comments
 * Values that are host addresses of an SFR or of a registered RAM region
 * are printed symbolically (&DMA0CNT(0x00231C), &buf+0x0), because the
 * host address is neither the target's nor deterministic.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"

#if TRACE_MODE == 3
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

_Static_assert(SFR_COUNT <= SFR_MEM_WORDS, "raise SFR_MEM_WORDS in sfr_host.h");

volatile uint32_t sfr_mem[SFR_MEM_WORDS] __attribute__((aligned(4096)));
static uint32_t shadow[SFR_MEM_WORDS];
static sfr_hook_t hooks[SFR_MEM_WORDS];
static unsigned long n_access;
static int log_reads;

static struct { uintptr_t lo; size_t n; const char *name; } regions[8];
static unsigned n_regions;

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

static unsigned pend_read_idx = ~0u;
static unsigned pend_read_n;

static void flush_reads(void)
{
    if (pend_read_n != 0u) {
        if (pend_read_n == 1u) { printf("R %s\n", sfr_info[pend_read_idx].name); }
        else { printf("R %s x%u\n", sfr_info[pend_read_idx].name, pend_read_n); }
        pend_read_n = 0u;
    }
}

static void log_write(unsigned i, uint32_t old, uint32_t now)
{
    char a[48], b[48];
    flush_reads();
    printf("W %-12s %s -> %s\n", sfr_info[i].name,
           trace_value(old, a, sizeof a), trace_value(now, b, sizeof b));
}

static void __attribute__((unused)) log_read(unsigned i)
{
    if (!log_reads) { return; }
    if (pend_read_n != 0u && pend_read_idx == i) { pend_read_n++; return; }
    flush_reads();
    pend_read_idx = i;
    pend_read_n = 1u;
}

/* ---- mode 3: page guard ------------------------------------------------ */
#if TRACE_MODE == 3
static int in_handler;
static unsigned pend_idx;
static int pend_kind;          /* 0 none, 1 read, 2 write */
static uint32_t pend_old;

static void guard(int on)
{
    DWORD old;
    if (!VirtualProtect((void *)sfr_mem, sizeof sfr_mem,
                        on ? PAGE_NOACCESS : PAGE_READWRITE, &old)) {
        fprintf(stderr, "VirtualProtect failed\n");
        exit(2);
    }
}

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
        uintptr_t lo = (uintptr_t)&sfr_mem[0];
        if (a < lo || a >= lo + sizeof sfr_mem) { return EXCEPTION_CONTINUE_SEARCH; }
        unsigned i = (unsigned)((a - lo) / 4u);
        guard(0);
        in_handler = 1;
        n_access++;
        pend_idx = i;
        pend_old = sfr_mem[i];
        if (er->ExceptionInformation[0] == 1) {
            pend_kind = 2;                              /* write (or RMW) */
        } else {
            pend_kind = 1;
            if (hooks[i]) { hooks[i](i); }              /* hardware answers */
        }
        in_handler = 0;
        ep->ContextRecord->EFlags |= 0x100u;            /* single step     */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && pend_kind != 0) {
        if (pend_kind == 2) {
            log_write(pend_idx, pend_old, sfr_mem[pend_idx]);
        } else {
            log_read(pend_idx);
        }
        shadow[pend_idx] = sfr_mem[pend_idx];
        pend_kind = 0;
        ep->ContextRecord->EFlags &= ~0x100u;
        guard(1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* ---- harness API ------------------------------------------------------- */
void trace_init(void)
{
    const char *e = getenv("TRACE_READS");
    log_reads = (e != NULL) && (e[0] == '1');
    memset((void *)sfr_mem, 0, sizeof sfr_mem);   /* reset values: all 0 in the spike */
    memset(shadow, 0, sizeof shadow);
#if TRACE_MODE == 3
    AddVectoredExceptionHandler(1, veh);
    guard(1);
#endif
}

void trace_flush(void)
{
#if TRACE_MODE == 3
    flush_reads();
#else
    for (unsigned i = 0; i < SFR_COUNT; i++) {
        if (sfr_mem[i] != shadow[i]) {
            log_write(i, shadow[i], sfr_mem[i]);
            shadow[i] = sfr_mem[i];
        }
    }
#endif
}

void trace_note(const char *fmt, ...)
{
    va_list ap;
    trace_flush();
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

unsigned trace_idx(const char *name)
{
    for (unsigned i = 0; i < SFR_COUNT; i++) {
        if (strcmp(sfr_info[i].name, name) == 0) { return i; }
    }
    fprintf(stderr, "trace_idx: no SFR %s\n", name);
    exit(2);
}

void trace_hook(unsigned idx, sfr_hook_t h) { hooks[idx] = h; }

void trace_region(const volatile void *p, size_t n, const char *name)
{
    regions[n_regions].lo = (uintptr_t)p;
    regions[n_regions].n = n;
    regions[n_regions].name = name;
    n_regions++;
}

uint32_t hw_get(unsigned idx)
{
#if TRACE_MODE == 3
    if (!in_handler) { guard(0); uint32_t v = sfr_mem[idx]; guard(1); return v; }
#endif
    return sfr_mem[idx];
}

void hw_set(unsigned idx, uint32_t v)
{
#if TRACE_MODE == 3
    if (!in_handler) { guard(0); sfr_mem[idx] = v; shadow[idx] = v; guard(1); return; }
#endif
    sfr_mem[idx] = v;                 /* hardware change: not a write */
    shadow[idx] = v;
}

unsigned long trace_accesses(void) { return n_access; }

#if TRACE_MODE == 2
volatile void *sfr_at(unsigned idx)
{
    trace_flush();                    /* the previous access's write, if any */
    n_access++;
    if (hooks[idx]) { hooks[idx](idx); }
    return &sfr_mem[idx];
}
#endif

/* __delay32(): into the trace, and Timer1 moves on by cycles/16. */
void __delay32(unsigned long cycles)
{
    trace_note("D __delay32(%lu)\n", cycles);
    unsigned t = trace_idx("TMR1");
    hw_set(t, hw_get(t) + (uint32_t)(cycles / 16u));
}

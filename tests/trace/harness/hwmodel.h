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
 * hwmodel.h - P0.5b register-trace harness: the deterministic read hook
 * that answers polling loops so approach (a) (no per-access read hook
 * for the TRACE itself, see tests/trace/README.md) still lets a driver's
 * busy-wait terminate - without a second OS thread.
 *
 * The rule shape is unchanged from P0.4/P0.5's background-thread model,
 * on purpose: `hwmodel_start()`/`hwmodel_stop()` keep their signatures so
 * every scenario's own rule table and call sites did not need to change
 * when P0.5b (27.09.2026, "the hybrid") replaced the mechanism behind
 * them. A rule still says "while this model runs, force `clear_mask`
 * bits of `reg` to 0 and `set_mask` bits to 1" - but P0.5b applies it at
 * a different moment: not continuously, from a second thread racing the
 * driver, but exactly once, synchronously, the instant the driver's own
 * code reads `reg` (a page-guard trap - VirtualProtect + a vectored
 * exception handler + single-step, tests/trace/README.md and the P0.3
 * spike's tests/trace/spike/recorder.c it is adapted from). The read
 * always sees the answer already applied, on its very first attempt -
 * there is no race left to mitigate, because there is no second thread
 * for the driver thread to race.
 *
 * TMR1 is handled the same way, by the same hook, keyed on its own index
 * instead of a rule: every read of TMR1 (via timebase_ticks()) advances
 * it by `tmr1_step` first, so a software wait on elapsed ticks
 * (chaintest.c's wait_ticks(), capture.c's two 25-tick waits) also
 * resolves in a small, deterministic number of reads instead of
 * depending on a background thread's wall-clock progress.
 */
#ifndef HWMODEL_H
#define HWMODEL_H

#include <stdint.h>

typedef struct {
    const char *reg;         /* SFR name, e.g. "PLL1CON"                */
    uint32_t    clear_mask;  /* bits forced to 0 on a read of reg       */
    uint32_t    set_mask;    /* bits forced to 1 on a read of reg       */
} hwmodel_rule_t;

/* Resolves every rule's register name to its sfr_mem[] index, works out
 * which page(s) of sfr_mem[] contain the named registers (and TMR1, if
 * tmr1_step != 0), page-guards exactly those pages (PAGE_NOACCESS) and
 * installs the vectored exception handler that answers a read of one of
 * them before the faulting instruction re-executes. `rules` must stay
 * valid until hwmodel_stop(). Call after trace_begin(). */
void hwmodel_start(const hwmodel_rule_t *rules, unsigned n_rules, uint32_t tmr1_step);

/* Un-guards every page hwmodel_start() guarded and removes the exception
 * handler. Call before trace_end(). A no-op if hwmodel_start() was never
 * called. */
void hwmodel_stop(void);

/* Temporarily un-guards (hwmodel_pause()) / re-guards (hwmodel_resume())
 * every page hwmodel_start() is currently guarding, without touching the
 * rule table or the exception handler. recorder.c's trace_flush() calls
 * these around its own diff loop, which otherwise reads every polled
 * register itself and fires its rule as if a driver had polled it - see
 * hwmodel.c. A no-op if hwmodel_start() was never called (or after
 * hwmodel_stop()), so recorder.c can call them unconditionally. */
void hwmodel_pause(void);
void hwmodel_resume(void);

#endif /* HWMODEL_H */

/*
 * chaintest_priv.h - private state and helpers shared with acquisition.c
 * only
 *
 * NOT part of chaintest.h's public API - nothing outside chaintest.c and
 * src/app/acquisition.c includes this. It exists because P9.4
 * (27.09.2026) moved chain_stream_on()/_on_input()/_off()/_streaming()/
 * _state()/_grab_begin()/_grab_end() into acquisition.c (the standing
 * stream the GUI drives is acquisition, not a test), but everything they
 * need to set the chain up stays in chaintest.c on purpose: setup()/
 * restore() also run every "chain all" stage and "chain run" (chain_all(),
 * chain_run(), both still in chaintest.c), and g_trig_hz is the one
 * trigger-frequency variable every stage of "chain all" reads and S1
 * measures - duplicating it for the standing stream would let the two
 * features disagree about the same hardware fact.
 *
 *   setup()/restore()      the clock tree and core switch, shared with
 *                           chain_all()/chain_run() (chaintest.c).
 *   triangle_for()          the DAC triangle sizing, shared with every
 *                           stage that starts the test signal.
 *   rate_hz()/ksps_of()     g_trig_hz arithmetic, shared with most stages.
 *   wait_ticks()            the bounded busy-wait, shared throughout.
 *   g_trig_hz               the measured (or nominal) trigger frequency;
 *                           chain_stream_on_input() resets it to
 *                           TRIG_HZ_NOMINAL exactly as "chain all" does at
 *                           the start of a run.
 *   s_core/s_pinsel/s_samc/  the input setup() applies; chain_stream_
 *   s_test_dac               on_input() sets them, calls setup(), then
 *                           restores the CHAIN_CORE/_PINSEL/_SAMC/true
 *                           defaults other callers of setup() rely on.
 *
 * All are non-static now, the same "plain extern" pattern capture_priv.h
 * uses for meter.c (P9.3) - not because acquisition.c owns them, but
 * because chaintest.c does and acquisition.c is the residual caller.
 *
 * CHAIN_CORE/_PINSEL/_SAMC, TRIG_HZ_NOMINAL, CPU_PER_TICK, TICKS_PER_MS
 * and CHAIN_ON_SIMULATOR are plain compile-time constants both files use;
 * defined once here instead of twice so they cannot drift apart.
 */
#ifndef CHAINTEST_PRIV_H
#define CHAINTEST_PRIV_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "timebase.h"

#define CHAIN_CORE        DAC_ADC_CORE      /* 5: DACOUT2 is AD5AN3        */
#define CHAIN_PINSEL      DAC_ADC_PINSEL    /* 3: RA8                      */
#define CHAIN_SAMC        0u                /* 0.5 TAD, the shortest       */
#define TRIG_HZ_NOMINAL   160000000u        /* CLKGEN13 = PLL1 out / 2     */
#define CPU_PER_TICK      16u               /* 200 MHz / 12.5 MHz          */
#define TICKS_PER_MS      (TIMEBASE_HZ / 1000u)

/* The simulator has no SCCP, ADC or DMA: a run-time test rather than
 * #ifdef, so that the simulator build still compiles - and warns about -
 * every line that checks it. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define CHAIN_ON_SIMULATOR  1
#else
#define CHAIN_ON_SIMULATOR  0
#endif

/* Measured in S1 (chaintest.c); reset to TRIG_HZ_NOMINAL at the start of
 * "chain all" and of chain_stream_on_input() alike. */
extern uint32_t g_trig_hz;

/* The input setup() (chaintest.c) applies; chain_stream_on_input()
 * (acquisition.c) sets these around its own call to setup(), then puts
 * the CHAIN_CORE/_PINSEL/_SAMC/true defaults back. */
extern uint8_t s_core, s_pinsel, s_samc;
extern bool    s_test_dac;

/* The clock tree and core 5 switch (chaintest.c): DMA down, SCCP1
 * stopped, half length at the maximum, PLL1 at 320 MHz, CLKGEN13 on, the
 * DAC's own clock selected, DAC2 started at mid-scale if s_test_dac, the
 * input selected, ADC in Single mode on the SCCP1 trigger. restore() puts
 * the boot configuration (board_cfg's PLL dividers, core burst mode) back
 * and clears the counters. */
bool setup(void);
void restore(void);

/* Pick SLPDAT so that one triangle slope lasts about SLOPE_TARGET samples
 * at `rate` Hz, the widest range the DAC's limits allow (chaintest.c). */
bool triangle_for(uint32_t rate, uint16_t *slp_out);

/* g_trig_hz arithmetic (chaintest.c): the sample rate an SCCP1 period of
 * n ticks gives, and the period n ticks corresponds to. */
uint32_t rate_hz(uint32_t n);
uint32_t ksps_of(uint32_t n);

/* Busy-wait t Timer1 ticks (chaintest.c). */
void wait_ticks(uint32_t t);

#endif /* CHAINTEST_PRIV_H */

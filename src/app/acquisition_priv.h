/*
 * acquisition_priv.h - private state and shared constants, chaintest.c is
 * the one file outside acquisition.c that reaches them
 *
 * NOT part of acquisition.h's public API - nothing outside acquisition.c
 * and src/tests/chaintest.c includes this. P9.4 (27.09.2026) had moved
 * chain_stream_on()/_on_input()/_off()/_streaming()/_state()/_grab_begin()/
 * _grab_end() into acquisition.c while leaving setup()/restore()/
 * triangle_for()/rate_hz()/ksps_of()/wait_ticks() in chaintest.c, reached
 * back through a chaintest_priv.h that pointed the wrong way: the
 * application layer (acquisition.c) depended on the test layer's internals
 * (chaintest.c), through non-static globals with generic names in the
 * whole firmware's namespace. P9.4b (27.09.2026) inverts it: the six
 * functions moved into acquisition.c, as acq_chain_setup()/acq_chain_
 * restore()/acq_triangle_for()/acq_rate_hz()/acq_ksps_of()/acq_wait_ticks()
 * (declared in acquisition.h - they are operations, not raw state, and
 * P11.3's routing_apply() is expected to call them too), and chaintest.c
 * now calls INTO acquisition.c (test -> app, the normal direction; see
 * CLAUDE.md's module table). What follows here is the raw state those six
 * functions and chaintest.c's own stages still need to read or write
 * directly - too raw for the public header, the same reasoning
 * capture_priv.h gives for clkdiv_cur:
 *
 *   acq_trig_hz        the measured (or nominal) trigger frequency;
 *                       acq_rate_hz()/acq_ksps_of() read it, acq_chain_
 *                       setup() and chain_stream_on_input() (acquisition.c)
 *                       reset it to TRIG_HZ_NOMINAL, and chaintest.c's S1
 *                       stage writes the measured value directly - the one
 *                       trigger-frequency variable every stage of
 *                       "chain all" reads, so as not to let the standing
 *                       stream and the chain test disagree about the same
 *                       hardware fact.
 *   acq_setup_rc_pll,   acq_chain_setup()'s own report (acquisition.c);
 *   acq_setup_trig,     chaintest.c's S0 stage reads them back to log what
 *   acq_setup_dac,      the last setup() call found, exactly as it always
 *   acq_setup_ok        has.
 *   acq_step_on         chaintest.c's CPU-stepped ADC flag (adc_ch0_event(),
 *                       S2/S3): acq_chain_restore() clears it defensively
 *                       along with everything else it resets, so it moved
 *                       with restore()'s body even though chaintest.c is
 *                       its only other reader/writer.
 *
 * s_core/s_pinsel/s_samc/s_test_dac (P9.4's chaintest_priv.h) needed no
 * such treatment: their only reader was setup() itself, now acq_chain_
 * setup() in acquisition.c, and their only writer was already chain_
 * stream_on_input() in acquisition.c - both ends of that state are in one
 * file now, so it went back to being a plain static there.
 *
 * CHAIN_CORE/_PINSEL/_SAMC, TRIG_HZ_NOMINAL, CPU_PER_TICK, TICKS_PER_MS
 * and CHAIN_ON_SIMULATOR are plain compile-time constants both files use;
 * defined once here instead of twice so they cannot drift apart - moved
 * verbatim from chaintest_priv.h, which this header replaces (deleted by
 * P9.4b).
 */
#ifndef ACQUISITION_PRIV_H
#define ACQUISITION_PRIV_H

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

/* Measured in chaintest.c's S1; reset to TRIG_HZ_NOMINAL by acq_chain_
 * setup() and by chain_stream_on_input() (both acquisition.c) alike. */
extern uint32_t acq_trig_hz;

/* acq_chain_setup()'s own report (acquisition.c), read back by chaintest.c's
 * S0 stage to log what the last call found. */
extern uint32_t acq_setup_rc_pll;
extern bool     acq_setup_trig, acq_setup_dac, acq_setup_ok;

/* chaintest.c's CPU-stepped ADC flag, set/cleared around its own S2/S3
 * windows; acq_chain_restore() (acquisition.c) also clears it, defensively,
 * along with everything else it puts back. */
extern volatile bool acq_step_on;

#endif /* ACQUISITION_PRIV_H */

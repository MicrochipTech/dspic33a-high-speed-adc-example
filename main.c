/*
 * main.c
 *
 * Start-up sequence and main loop of the ADC/DMA example on the EV74H48A
 * (dsPIC33 Curiosity Platform Development Board, dsPIC33AK512MPS512 DIM).
 *
 * The order below matters and is the whole story of this file:
 *
 *   1. LED0 first, so that a stop code can be shown from the very first
 *      checkpoint on.
 *   2. The UART, still on the 8 MHz FRC, so that every following step
 *      reports itself on the terminal and a failure inside clock_init()
 *      is readable, not just a blink code.
 *   3. Clocks: FRC -> PLL1 320 MHz for the ADC, PLL2 200 MHz for the CPU.
 *      Stops with blink code 1..4 if a step does not complete.
 *   4. The console proper: baud generator re-set for the 100 MHz
 *      peripheral clock, parser, banner, receive interrupt. It runs in
 *      the UART receive interrupt from here on.
 *   5. ADC core and DMA channel, armed but idle.
 *   6. Self-test on the ADC's internal 15/16 * VDD reference: the same
 *      chain as the measurement, only the input differs. Blink code 6, 7
 *      or 8 if it fails; the number is what tells the first person on the
 *      board where to look (docs/TROUBLESHOOTING.md).
 *   7. Measurement: the burst stream runs, the DMA interrupt keeps it
 *      going, the main loop processes each completed buffer half, blinks
 *      the heartbeat and prints a status line now and then, so a log of
 *      the terminal tells the story without anyone typing. The console
 *      can stop, start and reconfigure it at any time.
 *
 * Everything hardware-specific lives in the modules - board.h (pins,
 * ADC core), clock.c, adc.c, dma.c (sim_dma.c in the simulator build),
 * capture.c (counters, burst restart, self-test), led.c,
 * diag.c (stop codes, traps) and cli.c (UART, commands); this file only
 * sequences them.
 */

#include <xc.h>
#include "board.h"
#include "clock.h"
#include "adc.h"
#include "dma.h"
#include "capture.h"
#include "led.h"
#include "diag.h"
#include "timebase.h"
#include "dactest.h"
#include "dac.h"
#include "console.h"
#include "sim.h"

/* Status line every ~5 s for the first minute, then every ~60 s.
 * 39 062 halves per second at 40 MSPS. In the simulator a half costs a
 * few thousand instructions of the stand-in, so count halves, not time. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define STATUS_EVERY_HALVES   50u
#else
#define STATUS_EVERY_HALVES   195312u
#endif
#define STATUS_FAST_LINES     12u
#define IDLE_STATUS_TICKS     125000000u     /* 10 s of the 12.5 MHz time base */

/* Phase 2: the DAC2 triangle. 0x100..0xF00 (Example 18-3), SLPDAT 8 at
 * 320 MHz DAC clock = 22.4 us per slope, 44.8 us period, 22.3 kHz - the
 * piezo range. 64 halves = 65 536 samples, at 20 MSPS 73 periods. */
#define DACTEST_LOW           0x100u
#define DACTEST_HIGH          0xF00u
#define DACTEST_SLPDAT        8u
#define DACTEST_HALVES        64u

/* The automatic tests on the active ADC core: register snapshot, self-test
 * on the internal reference, pacing trial, rate sweep with the choice of
 * the fastest clean rate. Phase 1 runs it on the boot core (ADC_INSTANCE,
 * the mikroBUS input), phase 2 on ADC core 5 with DAC2 on its input. */
static void run_phase_tests(void)
{
    /* Register snapshot after initialisation, before anything runs: the
     * dump TROUBLESHOOTING.md Part 4 asks for, in every log, without
     * anyone typing "regs". */
    console_puts("[boot] register snapshot after init\r\n");
    regs_dump();

    console_puts("[boot] self-test on the internal reference\r\n");
    {
        const uint32_t rc = capture_selftest(NULL);
        if (rc != 0u) {
            fail(rc);
        }
    }

    /* Which trigger paces the conversions, and does the delivered rate
     * follow its period? ADC_PACING in board.h: AUTO tries every
     * candidate with the rate test and prints each verdict; a fixed one
     * is tested once and stops the boot with code 12 if it fails. */
    {
        const uint32_t rc = capture_autopace();
        if (rc != 0u) {
            fail(rc);
        }
    }

#if AUTO_SWEEP
    /* The rate sweep, once, without anyone typing (AUTO_SWEEP in
     * board.h): slowest to fastest, one line per rate - and the
     * measurement then runs at the fastest clean one. */
    console_puts("[boot] automatic rate sweep before the measurement (AUTO_SWEEP in board.h)\r\n");
    console_sweep(2000u, true);
#endif
}

#ifndef __MPLAB_DEBUGGER_SIMULATOR
/* What a phase ended with: pacing, period, nominal rate. */
static void phase_result(const char *tag)
{
    console_puts(tag); console_puts(" pacing chosen: "); console_puts(capture_pacing_name()); console_puts("\r\n");
    console_puts(tag); console_kv(" period", capture_period());
    console_puts(tag); console_kv(" ksps nominal", capture_nominal_ksps(capture_period()));
}
#endif

int main(void)
{
    /* Persistent RAM is undefined on the very first power-up (no start-up
     * code clears it, which is the point). A value outside the known
     * range means "no usable history", so normalise it before anything
     * reads it. */
    if (boot_stage > 9u) { boot_stage = 0u; trap_seen = 0u; trap_vec = 0u; trap_stage = 0u; }

    led_init();
    boot_mark(1u);
    console_early_init();
    boot_mark(2u);
    console_puts("[boot] " BUILD_ID "\r\n");
    SIM_BANNER();                     /* simulator build: say so first   */
    diag_report_reset();              /* why are we booting? RCON        */
    diag_report_build();              /* what runs: build, board, config */

    /* Did the previous run end in a trap? boot_stage/trap_* live in
     * persistent RAM, so say so now - an unhandled trap ends in "reset"
     * when no debugger is attached, and without this the board would just
     * appear to restart for no reason. */
    if (trap_seen != 0u) {
        console_puts("[boot] WARNING the previous run ended in a trap\r\n");
        console_kv("[boot] trap count", trap_seen);
        console_kv("[boot] last trap vector", trap_vec);
        console_kv("[boot] boot stage when it hit", trap_stage);
        console_puts("[boot] see [TRAP] in the earlier log, or docs/TROUBLESHOOTING.md 2.0b\r\n");
        trap_seen = 0u;          /* reported once; the next trap re-arms it */
    }

    boot_mark(3u);
    clock_init();
    boot_mark(4u);
    cli_init();
    boot_mark(5u);

    adc_init(ADC_PINSEL, ADC_SAMC, ADC_RPTCNT);
    boot_mark(6u);
    capture_init();
    boot_mark(7u);

    run_phase_tests();
    boot_mark(8u);

#ifdef __MPLAB_DEBUGGER_SIMULATOR
    /* The simulator's job is the ping-pong check, which needs the stream. */
    console_puts("[boot] self-test passed, measurement running on the external input\r\n");
    counters_clear();                 /* the self-test halves were not serviced */
    capture_start();
#else
    /* ---- phase 1 done: the boot core on the mikroBUS input ---------- */
    capture_shutdown();
    counters_clear();
    console_puts("\r\n"
                 "--------------------------------------------------------------\r\n");
    console_kv("[PHASE 1 DONE] tests on ADC core", adc_core());
    phase_result("[PHASE 1 DONE]");
    console_puts("--------------------------------------------------------------\r\n\r\n");

    /* ---- phase 2: ADC core 5 measuring DAC2 on the same pin (RA8) ---- */
    console_puts("[PHASE 2] ADC core 5, input AD5AN3 = RA8 = DACOUT2: DAC2 triangle on the pin, no wire\r\n");
    (void)capture_select_core(5u, 3u, ADC_SAMC);
    if (dac2_triangle_start(DACTEST_LOW, DACTEST_HIGH, DACTEST_SLPDAT)) {
        console_kv("[dac] DAC2 triangle on RA8, DACLOW", dac2_low());
        console_kv("[dac]   DACDAT", dac2_high());
        console_kv("[dac]   SLPDAT (counts per DAC clock)", dac2_slpdat());
        console_kv("[dac]   DAC clock Hz", clock_dac_hz());
        console_kv("[dac]   period ns", dac2_period_ns());
    } else {
        console_puts("[dac] CLKGEN7 did not come up - DAC2 is off, the DAC test will fail\r\n");
    }
    run_phase_tests();
    const uint32_t dac_rc = dactest_run(DACTEST_HALVES);

    /* ---- all done: everything off, the console has the CPU ---------- */
    dac2_off();
    capture_shutdown();
    counters_clear();
    /* Unmistakable end marker: the reader of a log must see at a glance
     * that every automatic test is over and what came out of it. */
    console_puts("\r\n"
                 "==============================================================\r\n"
                 "[DONE] ALL AUTOMATIC TESTS FINISHED (phase 1: boot core on the mikroBUS input, phase 2: ADC core 5 with DAC2)\r\n"
                 "[DONE] ADC core, CLKGEN6, DAC2 and CLKGEN7 are switched OFF - nothing converts\r\n");
    phase_result("[DONE] phase 2");
    console_puts(dac_rc == 0u ? "[DONE] DAC test: PASS - the DAC triangle arrived intact through ADC, DMA and the ping-pong buffer\r\n"
                              : "[DONE] DAC test: FAIL - see the [dactest] lines\r\n");
    console_puts("[DONE] the console is free now: type help. start = measure on the active core (5) at that rate; core 3 5 = back to the mikroBUS input\r\n"
                 "==============================================================\r\n\r\n");
#endif
    boot_mark(9u);
    led_mode(2u);                     /* heartbeat                       */

    uint32_t idle = 0;
    uint32_t next_status = STATUS_EVERY_HALVES;
    uint32_t status_lines = 0;
    uint32_t t_idle_status = timebase_ticks();   /* idle: a line per 10 s */

    for (;;) {
        SIM_DMA_TICK();               /* simulator: one half per pass    */
        if (capture_service()) {
            idle = 0;
            if (blocks_done >= next_status) {
                if (SIM_CHECK_RUNNING()) {
                    /* Simulator: the UART is slow, keep it quiet while
                     * the ping-pong check runs. Empty on silicon. */
                    next_status = blocks_done + STATUS_EVERY_HALVES;
                } else {
                    console_status_line();
                    console_half_stats();     /* is there a signal?  */
                    status_lines++;
                    next_status += (status_lines < STATUS_FAST_LINES)
                                   ? STATUS_EVERY_HALVES
                                   : 12u * STATUS_EVERY_HALVES;
                }
            }
        } else if (capture_running()) {
            /* The stream stopped: burst restart lost, or the DMA shut
             * itself off. Say so instead of sitting here silently. */
            if (!dma0_enabled())     { fail(8u); }
            if (++idle > WAIT_LIMIT) { fail(6u); }
        } else {
            idle = 0;                 /* stopped from the console        */
            /* Nothing running: a status line every 10 s anyway, so that
             * the log shows the console alive (rx counts) and the ADC
             * state (run=0, and after the boot powered=0). */
            if ((timebase_ticks() - t_idle_status) >= IDLE_STATUS_TICKS) {
                t_idle_status = timebase_ticks();
                console_status_line();
            }
        }

        /* What to look at with the debugger, the "status" command or the
         * [stat] lines:
         *   blocks_done   x SAMPLES_PER_HALF / elapsed time = actual rate
         *                 (includes the re-trigger gap once per buffer)
         *   dma_overrun   must stay 0, otherwise the DMA bus lost samples
         *   late_service  must stay 0, otherwise the ISR is too slow
         *   proc_missed   must stay 0, otherwise main() is too slow
         *   last_sample   changing means data is really moving
         *   selftest_mean ~3840 = the chain was proven before AN5 was used
         *   fail_code     0 while running; the LED pattern otherwise
         */
    }

    return 0;
}

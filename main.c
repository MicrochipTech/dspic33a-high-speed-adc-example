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
 * Everything hardware-specific lives in adc_dma_40msps.c (clock, ADC, DMA,
 * self-test, LED) and cli.c (UART, commands); this file only sequences it.
 */

#include <xc.h>
#include "adc_dma_40msps.h"

/* Status line every ~5 s for the first minute, then every ~60 s.
 * 39 062 halves per second at 40 MSPS. In the simulator a half costs a
 * few thousand instructions of the stand-in, so count halves, not time. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define STATUS_EVERY_HALVES   50u
#else
#define STATUS_EVERY_HALVES   195312u
#endif
#define STATUS_FAST_LINES     12u

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
    console_puts("[boot] adc_dma_40msps " __DATE__ " " __TIME__ "\r\n");

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

    adc_init(ADC_PINSEL, ADC_SAMC);
    boot_mark(6u);
    dma0_init();
    boot_mark(7u);

    console_puts("[boot] self-test on the internal reference\r\n");
    {
        const uint32_t rc = capture_selftest(NULL);
        if (rc != 0u) {
            fail(rc);
        }
    }
    boot_mark(8u);

    console_puts("[boot] self-test passed, measurement running on the external input\r\n");
    capture_start();
    boot_mark(9u);
    led_mode(2u);                     /* heartbeat                       */

    uint32_t idle = 0;
    uint32_t next_status = STATUS_EVERY_HALVES;
    uint32_t status_lines = 0;

    for (;;) {
        SIM_DMA_TICK();               /* simulator: one half per pass    */
        if (capture_service()) {
            idle = 0;
            if (blocks_done >= next_status) {
                console_status_line();
                status_lines++;
                next_status += (status_lines < STATUS_FAST_LINES)
                               ? STATUS_EVERY_HALVES
                               : 12u * STATUS_EVERY_HALVES;
            }
        } else if (capture_running()) {
            /* The stream stopped: burst restart lost, or the DMA shut
             * itself off. Say so instead of sitting here silently. */
            if (!dma0_enabled())     { fail(8u); }
            if (++idle > WAIT_LIMIT) { fail(6u); }
        } else {
            idle = 0;                 /* stopped from the console        */
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

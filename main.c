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
 *   2. Clocks: FRC -> PLL1 320 MHz for the ADC, PLL2 200 MHz for the CPU.
 *      Stops with blink code 1..4 if a step does not complete.
 *   3. The console (UART1 on the PKOB4 USB-UART channel). It runs in the
 *      UART receive interrupt from here on and prints its banner now, so
 *      a stop code later is preceded by something readable on the
 *      terminal. It needs the 100 MHz peripheral clock for its baud rate,
 *      hence after the clocks.
 *   4. ADC core and DMA channel, armed but idle.
 *   5. Self-test on the ADC's internal 15/16 * VDD reference: the same
 *      chain as the measurement, only the input differs. Blink code 6, 7
 *      or 8 if it fails; the number is what tells the first person on the
 *      board where to look (docs/TROUBLESHOOTING.md).
 *   6. Measurement: the burst stream runs, the DMA interrupt keeps it
 *      going, the main loop processes each completed buffer half and
 *      blinks the heartbeat. The console can stop, start and reconfigure
 *      it at any time.
 *
 * Everything hardware-specific lives in adc_dma_40msps.c (clock, ADC, DMA,
 * self-test, LED) and cli.c (UART, commands); this file only sequences it.
 */

#include <xc.h>
#include "adc_dma_40msps.h"

int main(void)
{
    led_init();
    clock_init();
    cli_init();

    adc_init(ADC_PINSEL, ADC_SAMC);
    dma0_init();

    {
        const uint32_t rc = capture_selftest(NULL);
        if (rc != 0u) {
            fail(rc);
        }
    }

    capture_start();
    led_mode(2u);                     /* heartbeat                       */

    uint32_t idle = 0;
    for (;;) {
        if (capture_service()) {
            idle = 0;
        } else if (capture_running()) {
            /* The stream stopped: burst restart lost, or the DMA shut
             * itself off. Say so instead of sitting here silently. */
            if (!dma0_enabled())     { fail(8u); }
            if (++idle > WAIT_LIMIT) { fail(6u); }
        } else {
            idle = 0;                 /* stopped from the console        */
        }

        /* What to look at with the debugger or the "status" command:
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

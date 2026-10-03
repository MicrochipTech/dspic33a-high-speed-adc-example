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
 * example_main.c - the customer's starting point (CORE.5, 02.10.2026): the
 * core build's main(), linked by "tools\build.bat core" instead of
 * src/app/main.c. It brings the clock tree, the console and the ADC/DMA
 * chain up and then serves the ping-pong buffer - nothing of src/lab/ is
 * linked, and nothing converts until a command says so:
 *
 *   stream on <ksps> [core pinsel [samc]]   the triggered stream (acquisition.c)
 *   stream grab                             one half as a binary frame (the GUI)
 *   sigproc on|off                          your processing, in place
 *   siggen ...                              the signal generator (siggen.c)
 *   help                                    everything else
 *
 * Where your code goes: sigproc_block() in sigproc.c is called by
 * capture_service() below with every completed half (pointer, length, and
 * the sample rate in sigproc_info_t) while "sigproc on". It runs in this
 * loop, not in an interrupt; it must finish within one half period or the
 * next half is counted as "missed" (status, the GRAB frame's missed=/load=).
 * To start the stream without a command, call chain_stream_on(ksps) after
 * capture_init() - see EXAMPLE_START_KSPS.
 *
 * src/app/main.c is the lab's main(): the same start-up plus the boot report
 * of the chain test, the simulator's ping-pong check and smoke script, and
 * the back-to-back mode's [stat] lines.
 */
#include <xc.h>
#include "board.h"
#include "clock.h"
#include "adc.h"
#include "capture.h"
#include "led.h"
#include "diag.h"
#include "timebase.h"
#include "console.h"

/* Stream at this rate from boot (kSPS, 0 = wait for "stream on"). */
#define EXAMPLE_START_KSPS  0u

int main(void)
{
    /* Persistent RAM holds garbage after a power-up (diag.h). */
    if (boot_stage > 9u) { boot_stage = 0u; trap_seen = 0u; trap_vec = 0u; trap_stage = 0u; }
    diag_stack_paint();               /* first: "status" reports the stack's high-water mark */
    led_init();
    boot_mark(1u);

    console_early_init();             /* UART2 on the FRC, before the clocks */
    boot_mark(2u);
    console_puts("\r\n[boot] " BUILD_ID " (core build)\r\n");
    diag_report_reset();              /* why are we booting? RCON */
    boot_mark(3u);

    clock_init();                     /* PLLs, CPU clock, clock-fail interrupt */
    boot_mark(4u);
    cli_init();                       /* console on the PLL clock, parser, commands */
    boot_mark(5u);
    timebase_init();
    adc_init(ADC_PINSEL, ADC_SAMC, SAMPLES_PER_BUF_MAX);
    boot_mark(6u);
    capture_init();                   /* DMA buffer, guard words, counters */
    boot_mark(7u);
    (void)capture_set_pll(board_cfg.adc_pll_postdiv1, board_cfg.adc_pll_postdiv2);
    boot_mark(8u);

#if EXAMPLE_START_KSPS > 0u
    if (!chain_stream_on(EXAMPLE_START_KSPS)) {
        console_puts("[boot] stream on failed\r\n");
    }
#endif
    console_puts("[boot] READY - 'stream on <ksps>' starts the stream, 'help' lists the commands\r\n");
    boot_mark(9u);
    led_mode(2u);                     /* heartbeat */

    for (;;) {
        (void)capture_service();      /* a completed half: bookkeeping, sigproc_block() */
        console_rx_resume();          /* console bytes held back while a half was processed */
    }

    return 0;
}

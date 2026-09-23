/*
 * board.h
 *
 * Everything that ties the ADC/DMA example to the EV74H48A (dsPIC33
 * Curiosity Platform Development Board) with the dsPIC33AK512MPS512 GP
 * DIM: which ADC core and input, the LED pin, the console pins. Change
 * this file for another board or another input; the modules do not care.
 */
#ifndef BOARD_H
#define BOARD_H

/* ADC core and input. EV74H48A defaults: ADC3, AD3AN5 = mikroBUS A pin AN
 * (device pin RA0). The on-board potentiometer is AD5AN0 (RA7): set
 * ADC_INSTANCE 5, ADC_PINSEL 0 and a longer sample time for that. */
#ifndef ADC_INSTANCE
#define ADC_INSTANCE      3
#endif
#ifndef ADC_PINSEL
#define ADC_PINSEL        5u
#endif
#ifndef ADC_SAMC
#define ADC_SAMC          0u      /* sample time 0.5 TAD                      */
#endif
/* Sample rate: the ADC's repeat timer triggers a conversion every
 * ADC_RPTCNT TAD (TAD = 12.5 ns with the 320 MHz ADC clock). 2 = 40 MSPS,
 * 4 = 20 MSPS, 8 = 10 MSPS, 63 = 1.27 MSPS. The measured rate is in the
 * sweep table; "period <n>" changes it at run time. */
#ifndef ADC_RPTCNT
#define ADC_RPTCNT        2u
#endif

/* What paces the conversions inside a burst (TRG2SRC, DS70005591D Table
 * 16-4 p1227). Three candidates, and the boot can try them in turn:
 *   3   the ADC's repeat timer, period ADC_RPTCNT TAD (12.5 ns)
 *   32  SCCP1 timer period match, period ADC_SCCP_TICKS x 10 ns - what
 *       datasheet Example 16-8 shows with Integration mode
 *   2   back-to-back, as fast as the converter goes - no rate control;
 *       what Microchip's 40 MSPS example uses
 *   0   AUTO: try 3, then 32, each with the rate test; the first that
 *       delivers the rate its period says wins; if neither does, 2.
 *       The log says which ("[pacing] ..."). The default, because the
 *       repeat-timer pairing rests on the datasheet text alone and the
 *       board has the last word.
 * A fixed value skips the trial; the rate test then stops the boot with
 * fail 12 if that source does not deliver. */
#ifndef ADC_PACING
#define ADC_PACING        0u
#endif
#ifndef ADC_SCCP_TICKS
#define ADC_SCCP_TICKS    5u      /* 5 x 10 ns = 20 MSPS                      */
#endif

/* Boot chatter: with 1 every start-up step reports its registers on the
 * console ([clk] PLL1 locked, [adc] pinsel, [dma] DMALOW ...). With 0 the
 * boot prints only what changes from run to run - reset cause, self-test
 * and rate-test results, the sweep, and anything that fails. fail() and
 * the trap handler print everything either way. */
#ifndef BOOT_VERBOSE
#define BOOT_VERBOSE      0
#endif

/* Run the rate sweep (the "sweep" console command) once automatically,
 * right after the self-test and before the measurement starts. Needs no
 * console input: the table appears on the terminal by itself, from the
 * slowest rate to the fastest, so a board that dies at some rate shows
 * where. 0 = only on command. */
#ifndef AUTO_SWEEP
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define AUTO_SWEEP        0       /* 36 000 halves at ~3 per second: no    */
#else
#define AUTO_SWEEP        1
#endif
#endif

/* LED0 on the Curiosity Platform Development Board is RC8, DIM pin 28
 * (DIM info sheet DS70005563A, Table 1). The green LEDs are driven high
 * to light (user guide DS70005562D 2.5; Microchip's own example on this
 * board reports "LED0 HIGH during sampling"). Port C has no ANSEL. */
#define LED_TRIS          TRISCbits.TRISC8
#define LED_LAT           LATCbits.LATC8

/* Console: UART2 on the MCP2221A USB-UART channel. U2TX -> RH1 (RP114,
 * DIM pin P98 "UART_USB_TX"), U2RX <- RD1 (RP50, DIM pin P96
 * "UART_USB_RX"). PPS codes: U2TX output function 21 (Table "Output
 * Selection for Remappable Pins", p613). Neither port has an analog
 * function. */
#define CONSOLE_TX_TRIS   TRISHbits.TRISH1
#define CONSOLE_RX_TRIS   TRISDbits.TRISD1
#define CONSOLE_RX_RPINR  _U2RXR
#define CONSOLE_RX_RP     50u             /* RP50  -> U2RX */
#define CONSOLE_TX_RPOR   _RP114R
#define CONSOLE_TX_FN     21u             /* RP114 <- U2TX */


/* Name of the board for the banner and the "version" command. */
#define BOARD_NAME        "EV74H48A, dsPIC33AK512MPS512 GP DIM"

/* Git revision of the working tree, written to version.h by
 * tools/version.bat (MPLAB X pre-build step, build.bat) or version.sh
 * (Makefile) before every build. A build that skipped the script says
 * "unknown" - then the banner's date and time are all there is. */
#ifdef __has_include
# if __has_include("version.h")
#  include "version.h"
# endif
#endif
#ifndef GIT_REV
#define GIT_REV           "unknown"
#endif
#ifndef GIT_BRANCH
#define GIT_BRANCH        "unknown"
#endif
#ifndef GIT_DIRTY
#define GIT_DIRTY         0
#endif
#if GIT_DIRTY
#define GIT_DIRTY_TAG     "+local changes"
#else
#define GIT_DIRTY_TAG     ""
#endif
/* One line that identifies the firmware: what was built, when, from
 * which commit. First line of every log. */
#define BUILD_ID          "adc_dma_40msps " __DATE__ " " __TIME__ \
                          " git " GIT_REV GIT_DIRTY_TAG " (" GIT_BRANCH ")"

#endif /* BOARD_H */

/*
 * board.h
 *
 * Everything that ties the ADC/DMA example to a board: which ADC core
 * and input, the LED pin and its polarity, the console pins, the
 * device. Two boards are known; BOARD selects one (the MPLAB X
 * configurations and build.bat set it, the default is the EV74H48A):
 *
 *   BOARD_EV74H48A  dsPIC33 Curiosity Platform Development Board with the
 *                   dsPIC33AK512MPS512 GP DIM (user guide DS70005562D, DIM
 *                   info sheet DS70005563A). Console on the MCP2221A
 *                   USB-UART channel, input on mikroBUS A.
 *   BOARD_EV17P63A  dsPIC33AK512MPS506 Curiosity Nano (user guide
 *                   DS70005634). Console on the debugger's CDC channel,
 *                   input on the edge connector.
 *
 * The modules never look at BOARD; they use the macros below. The two
 * devices share the ADC, the DMA, the clock tree and the RAM map, so
 * the rest of the code is the same for both - only the device the
 * compiler is told about (-mcpu, the project's target device) differs.
 */
#ifndef BOARD_H
#define BOARD_H

#define BOARD_EV74H48A    1
#define BOARD_EV17P63A    2

#ifndef BOARD
#define BOARD             BOARD_EV74H48A
#endif

#if BOARD == BOARD_EV74H48A
/* ---------------------------------------------------------------- */
#define BOARD_NAME        "EV74H48A, dsPIC33AK512MPS512 GP DIM"
#if !defined(__dsPIC33AK512MPS512__)
#warning "BOARD_EV74H48A carries a dsPIC33AK512MPS512 - is the project's device right?"
#endif

/* ADC core and input: ADC3, AD3AN5 = mikroBUS A pin AN (device pin
 * RA0). The on-board potentiometer is AD5AN0 (RA7): set ADC_INSTANCE 5,
 * ADC_PINSEL 0 and a longer sample time for that. */
#ifndef ADC_INSTANCE
#define ADC_INSTANCE      3
#endif
#ifndef ADC_PINSEL
#define ADC_PINSEL        5u
#endif
#define BOARD_INPUT_NAME  "AD3AN5 = mikroBUS A pin AN (RA0)"

/* LED0 is RC8, DIM pin 28 (DS70005563A Table 1), driven HIGH to light
 * (DS70005562D 2.5). Port C has no ANSEL. */
#define LED_TRIS          TRISCbits.TRISC8
#define LED_LAT           LATCbits.LATC8
#define LED_ACTIVE_LOW    0

/* Console: UART2 on the MCP2221A USB-UART channel. U2TX -> RH1 (RP114,
 * DIM pin P98 "UART_USB_TX"), U2RX <- RD1 (RP50, DIM pin P96
 * "UART_USB_RX"). PPS: U2TX output function 21 (p613), input remap
 * numbers from Table 11-19 (p611). Neither port has an analog function. */
#define CONSOLE_TX_TRIS   TRISHbits.TRISH1
#define CONSOLE_RX_TRIS   TRISDbits.TRISD1
#define CONSOLE_RX_RPINR  _U2RXR
#define CONSOLE_RX_RP     50u             /* RP50  -> U2RX */
#define CONSOLE_TX_RPOR   _RP114R
#define CONSOLE_TX_FN     21u             /* RP114 <- U2TX */
#define CONSOLE_PORT_NAME "UART2 on the MCP2221A USB-UART channel (RH1/RD1)"
/* The whole registers behind the pins, for the register dump: RPOR28
 * holds RP112..115 (RP114R in bits 23:16), RPINR13 holds U2RXR. */
#define CONSOLE_TX_TRIS_WORD  TRISH
#define CONSOLE_RX_TRIS_WORD  TRISD
#define CONSOLE_TX_RPOR_WORD  RPOR28
#define CONSOLE_RX_RPINR_WORD RPINR13

#elif BOARD == BOARD_EV17P63A
/* ---------------------------------------------------------------- */
#define BOARD_NAME        "EV17P63A, dsPIC33AK512MPS506 Curiosity Nano"
#if !defined(__dsPIC33AK512MPS506__)
#warning "BOARD_EV17P63A carries a dsPIC33AK512MPS506 - is the project's device right?"
#endif

/* ADC core and input: ADC1, AD1AN0 = RA2 (RP3, QFN64 pin 12, shared
 * with OA1OUT/CMP1A; the op amp is off after reset), on the edge
 * connector, labelled "RA2 / AD1AN0" (DS70005634 4.2, pin table). Port A
 * ANSEL resets to analog (DS70005591D 11.3.6, p640: reset 1 = digital
 * Schmitt trigger disabled), so nothing to set. */
#ifndef ADC_INSTANCE
#define ADC_INSTANCE      1
#endif
#ifndef ADC_PINSEL
#define ADC_PINSEL        0u
#endif
#define BOARD_INPUT_NAME  "AD1AN0 = edge connector RA2"

/* LED0 is RD0 (RP49), active LOW: driving the pin low lights it
 * (DS70005634 4.2.2). Port D has no ANSEL. */
#define LED_TRIS          TRISDbits.TRISD0
#define LED_LAT           LATDbits.LATD0
#define LED_ACTIVE_LOW    1

/* Console: UART2 on the on-board debugger's CDC channel. DS70005634
 * Table 4-x / 6.2: RC10 = RP43 is "UART TX (dsPIC33AK512MPS506 TX
 * line)" = the debugger's CDC RX; RC11 = RP44 is "UART RX (... RX line)"
 * = the debugger's CDC TX. So U2TX -> RC10, U2RX <- RC11. The UART
 * instance stays 2 - only the pins differ from the other board. Port C
 * has no ANSEL. */
#define CONSOLE_TX_TRIS   TRISCbits.TRISC10
#define CONSOLE_RX_TRIS   TRISCbits.TRISC11
#define CONSOLE_RX_RPINR  _U2RXR
#define CONSOLE_RX_RP     44u             /* RP44 (RC11) -> U2RX */
#define CONSOLE_TX_RPOR   _RP43R
#define CONSOLE_TX_FN     21u             /* RP43 (RC10) <- U2TX */
#define CONSOLE_PORT_NAME "UART2 on the debugger's CDC channel (RC10/RC11)"
/* For the register dump: RPOR10 holds RP40..43 (RP43R in bits 31:24),
 * RPINR13 holds U2RXR; both console pins are on port C. */
#define CONSOLE_TX_TRIS_WORD  TRISC
#define CONSOLE_RX_TRIS_WORD  TRISC
#define CONSOLE_TX_RPOR_WORD  RPOR10
#define CONSOLE_RX_RPINR_WORD RPINR13

#else
#error "BOARD must be BOARD_EV74H48A or BOARD_EV17P63A"
#endif

/* ---- Board-independent choices ---------------------------------- */

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
 *   34  SCCP1 trigger as the burst's second trigger, period
 *       ADC_SCCP_TICKS x 10 ns (Table 16-4 code 100010; the 32 used
 *       until 23.09. evening was "PTG trigger 12")
 *   64  the ADC clock itself: back-to-back conversions, rate set by the
 *       CLKGEN6 divider (period = divide ratio of the 320 MHz clock in
 *       hundredths, 100..1000: 100 = 40 MSPS, 200 = 20, 250 = 16, 500 =
 *       8, 1000 = 4 MSPS; the sweep runs the even ratios first, then a
 *       second pass with fractional ones). Not a TRG2SRC value; clock.c
 *       does the switching. The board showed on 23.09.
 *       (HARDWARE-LOG run 6) that neither 3 nor 32 paces a burst in
 *       Integration mode, so this is the source that actually works.
 *   2   back-to-back, as fast as the converter goes - no rate control;
 *       what Microchip's 40 MSPS example uses
 *   65  one conversion per SCCP1 trigger: Single Conversion mode, the
 *       SCCP1 trigger as TRG1SRC, period ADC_SCCP_TICKS x 10 ns. No
 *       burst, no restart - the mechanism of Microchip's own 40 MSPS
 *       example (8 channels x 5 MSPS, each one paced exactly so).
 *   0   AUTO: try 65, 64, 3, 34, each with the rate test; the first that
 *       delivers the rate its period says wins; if none does, 2.
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
#ifndef ADC_CLKDIV
#define ADC_CLKDIV        100u    /* ADC clock divide ratio x 100: 100 = 320 MHz */
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

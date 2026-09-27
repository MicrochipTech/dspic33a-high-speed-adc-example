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
/* Sample rate. The conversions run back-to-back - one after the other,
 * as fast as the converter goes - and the only thing that changes the
 * rate is the ADC clock itself, the CLKGEN6 divider. Nothing else on
 * this silicon paces them: the ADC repeat timer, the SCCP1 trigger and
 * the sample time SAMC were all tried on the board and all ignored
 * (docs/HARDWARE-LOG.md runs 4 to 7), so they are gone from this code.
 *
 * The ratio is in hundredths of the 320 MHz clock, and eight clocks make
 * one conversion: 1000 = /10 = 32 MHz = 4 MSPS, 500 = /5 = 64 MHz =
 * 8 MSPS, 100 = 320 MHz = 40 MSPS. 32 MHz is the ADC minimum (Table
 * 16-1), so 1000 is the slowest setting there is.
 *
 * The default is that slowest setting on purpose: it is the one rate the
 * DMA should manage comfortably, so the first test of a run is the one
 * most likely to pass, and a failure there means the chain itself is
 * broken - not the rate. "clk <ratio>" changes it at run time. */
#ifndef ADC_CLKDIV
#define ADC_CLKDIV        100u    /* CLKGEN6 divider: straight through       */
#endif

/* The sample rate at boot, as PLL1's two output dividers: the ADC clock
 * is 1600 MHz / (POSTDIV1 * POSTDIV2), and eight of those clocks make one
 * back-to-back conversion. 7/7 = 32.65 MHz = 4.08 MSPS is the slowest
 * setting that still clears the ADC's 32 MHz minimum; 5/5 = 64 MHz =
 * 8 MSPS is what the customer's application needs; 5/1 = 320 MHz =
 * 40 MSPS is the maximum and what clock_init() starts with.
 *
 * The slowest setting is the default on purpose: it is the one the DMA
 * should manage comfortably, so the first test of a run is the one most
 * likely to pass, and a failure there means the chain itself is broken -
 * not the rate. "pll <p1> <p2>" changes it at run time.
 *
 * Why the PLL and not the CLKGEN6 divider: in runs 8 and 9 the divider
 * seemed to have no effect - but both runs measured under overrun load,
 * an instrument later found void, and every document names CLKGEN6 as
 * the ADC clock (ANALYSIS.md C.3, withdrawn 25.09.2026). The chain test
 * measures it at the generator itself (chaintest.c S8). The chain test
 * does not use this boot rate: it sets PLL1 to 5/1 and paces the ADC
 * with SCCP1. */
#ifndef ADC_PLL_POSTDIV1
#define ADC_PLL_POSTDIV1  7u
#endif
#ifndef ADC_PLL_POSTDIV2
#define ADC_PLL_POSTDIV2  7u
#endif

/* Boot chatter: with 1 every start-up step reports its registers on the
 * console ([clk] PLL1 locked, [adc] pinsel, [dma] DMALOW ...). With 0 the
 * boot prints only what changes from run to run - reset cause and
 * anything that fails. fail() and
 * the trap handler print everything either way. */
#ifndef BOOT_VERBOSE
#define BOOT_VERBOSE      0
#endif

/* Where the DAC lands in the ADC, from the pinout table (DS70005591D
 * Table 1, 100-pin and 128-pin columns):
 *
 *   PGC2/DACOUT1/AD5AN1/CVDAN1/CMP4D/RP2/RA1        DACOUT1 = AD5AN1
 *   DACOUT2/AD5AN3/CVDAN8/CMP5A/IBIAS3/ISRC3/RA8    DACOUT2 = AD5AN3
 *
 * BOTH DAC OUTPUTS BELONG TO ADC CORE 5. The DAC test therefore has to
 * switch the core, whatever core the rest of the run uses - run 10
 * (24.09.2026) ran it on core 3, which cannot see RA8 at all, and
 * measured the open mikroBUS pin instead of the triangle.
 *
 * DAC2 is the one to use: DAC1 shares its pin with PGC2, the second
 * programming clock. On the EV74H48A, RA8 is DIM pin P44 and goes to
 * capacitive touch pad 2 - the loop closes on the pin, no wire needed. */
#define DAC_ADC_CORE      5u
#define DAC_ADC_PINSEL    3u      /* AD5AN3 = RA8 = DACOUT2, the pin route   */

/* The internal route, and the one the DAC test uses: UREFCON.INSEL = 7
 * puts DAC2 on the chip's UREF line, and ADnAN7 is the UREF input of
 * EVERY core (Table 16-2). So the ADC measures the DAC inside the chip -
 * no pin, no wire, no core switch, and none of the loading the board's
 * touch-pad network puts on RA8. */
#define DAC_UREF_PINSEL   7u      /* ADnAN7 = UREF input, any core           */

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

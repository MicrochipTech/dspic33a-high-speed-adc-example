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
 * Why the PLL and not the CLKGEN6 divider: the divider does not work.
 * Every ratio was written, read back and confirmed by DIVSWEN and CLKRDY,
 * with the generator switched off around the write and with it left
 * running, and the ADC converted at 40 MSPS at every one of them
 * (docs/HARDWARE-LOG.md runs 8 and 9). */
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

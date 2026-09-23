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
#define ADC_SAMC          0u      /* 0.5 TAD, 40 MSPS at 320 MHz input clock */
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

#endif /* BOARD_H */

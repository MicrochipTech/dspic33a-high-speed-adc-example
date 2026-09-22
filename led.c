/*
 * led.c
 *
 * LED0 of the ADC/DMA example: the one status output that works with no
 * clock, no console and no debugger. The pin is in board.h.
 *
 * What you see on the board
 *   Slow blink (1 Hz) = everything runs and no error counter has moved.
 *   Fast blink (5 Hz) = running, but an error counter is non-zero
 *   (both from capture_service()). A counted blink pattern with a pause =
 *   the code stopped at a checkpoint; the count is the error code (table
 *   at fail() in diag.c, also in docs/TROUBLESHOOTING.md).
 */

#include <xc.h>
#include "board.h"
#include "led.h"

/* LED0 on the Curiosity Platform Development Board is RC8, DIM pin 28
 * (DIM info sheet DS70005563A, Table 1). The green LEDs are driven high
 * to light (user guide DS70005562D 2.5; Microchip's own example on this
 * board reports "LED0 HIGH during sampling"). Port C has no ANSEL. */
#define LED_TRIS          TRISCbits.TRISC8
#define LED_LAT           LATCbits.LATC8
#define LED_ON()          (LED_LAT = 1u)
#define LED_OFF()         (LED_LAT = 0u)
#define LED_TOGGLE()      (LED_LAT = (uint8_t)!LED_LAT)


static volatile uint8_t led_auto = 2u;   /* 0 off, 1 on, 2 auto */

void led_init(void)
{
    LED_OFF();
    LED_TRIS = 0u;
}

/* on/off also re-assert the direction: the trap handler calls these
 * assuming nothing about the state the port was left in. */
void led_on(void)     { LED_ON();  LED_TRIS = 0u; }
void led_off(void)    { LED_OFF(); LED_TRIS = 0u; }
void led_toggle(void) { LED_TOGGLE(); }

void led_mode(uint8_t mode)
{
    led_auto = mode;
    if (mode == 0u)      { LED_OFF(); }
    else if (mode == 1u) { LED_ON();  }
}

uint8_t led_get_mode(void) { return led_auto; }

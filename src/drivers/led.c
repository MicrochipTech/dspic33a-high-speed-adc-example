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

/* Pin and polarity come from board.h: RC8 driven high on the EV74H48A,
 * RD0 driven low on the Curiosity Nano. */
#if LED_ACTIVE_LOW
#define LED_ON()          (LED_LAT = 0u)
#define LED_OFF()         (LED_LAT = 1u)
#else
#define LED_ON()          (LED_LAT = 1u)
#define LED_OFF()         (LED_LAT = 0u)
#endif
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

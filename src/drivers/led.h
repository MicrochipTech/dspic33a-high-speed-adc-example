/*
 * led.h - LED0 of the ADC/DMA example (led.c)
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

/* LED0 off and configured as output. */
void led_init(void);

/* Direct control; on/off also force the pin to output, so they work from
 * a trap handler that assumes nothing. */
void led_on(void);
void led_off(void);
void led_toggle(void);

/* LED0: 0 = off, 1 = on, 2 = automatic (heartbeat while running). */
void    led_mode(uint8_t mode);
uint8_t led_get_mode(void);

#endif /* LED_H */

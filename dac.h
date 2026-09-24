/*
 * dac.h - DAC2 as a signal source for the ADC test (dac.c)
 *
 * DAC2 is one of the two DACs with an output buffer to a pin: DACOUT2 =
 * RA8 (128-pin device pin 7; DIM pin P44 = capacitive touch pad 2 on the
 * EV74H48A). RA8 is also AD5AN3, so ADC core 5 can measure the DAC on the
 * same pin without a wire. Triangle Wave mode (DS70005591D 18.4.x,
 * Example 18-3): the hardware ramps between DACLOW and DACDAT at SLPDAT
 * counts per DAC clock, no CPU involved. Slope duration per Equation
 * 18-4: T = (DACDAT - DACLOW) * 16 * T_DAC / SLPDAT; a period is two
 * slopes. DAC clock = CLKGEN7 (clock.c).
 */
#ifndef DAC_H
#define DAC_H

#include <stdbool.h>
#include <stdint.h>

/* Triangle between the DAC codes low and high (0xCD + SLPDAT .. 0xF32 -
 * SLPDAT, note 1 of Example 18-3; 12-bit, VDD-referred like the ADC),
 * SLPDAT counts per DAC clock. Enables CLKGEN7, the DAC and the pin
 * buffer. False if the clock did not come up. */
bool     dac2_triangle_start(uint16_t low, uint16_t high, uint16_t slpdat);
void     dac2_off(void);
bool     dac2_running(void);
uint16_t dac2_low(void);
uint16_t dac2_high(void);
uint16_t dac2_slpdat(void);
uint32_t dac2_period_ns(void);         /* of the triangle, from the settings */

void     dac_regs_dump(void);

#endif /* DAC_H */

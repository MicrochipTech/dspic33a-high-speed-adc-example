/*
 * dac.h - DAC1 and DAC2 as signal sources for the ADC test (dac.c)
 *
 * The device has eight 12-bit PDM DACs, one per analog comparator, and two
 * of them have an output buffer to a pin: DACOUT1 = RA1 and DACOUT2 = RA8
 * (128-pin device pins 7 and 14, 64-pin pins 3 and 7). Both pins are also
 * ADC inputs of core 5 - RA1 is AD5AN1, RA8 is AD5AN3 - so core 5 can read
 * either DAC back on its own pin, with no wire. Any other ADC input needs
 * one: the GUI's board tile shows which two pads to connect.
 *
 * Triangle Wave mode (DS70005591D 18.4.x, Example 18-3): the hardware
 * ramps between DACLOW and DACDAT at SLPDAT counts per DAC clock, no CPU
 * involved. Slope duration per Equation 18-4: T = (DACDAT - DACLOW) * 16 *
 * T_DAC / SLPDAT; a period is two slopes. DAC clock = CLKGEN7 (clock.c).
 */
#ifndef DAC_H
#define DAC_H

#include <stdbool.h>
#include <stdint.h>

#define DAC_UNITS   2                  /* the two with an output pin      */

/* Triangle between the DAC codes low and high (0xCD + SLPDAT .. 0xF32 -
 * SLPDAT, note 1 of Example 18-3; 12-bit and VDD-referred, like the ADC),
 * SLPDAT counts per DAC clock. Enables CLKGEN7, the DAC and its pin
 * buffer. `unit` is 1 or 2. False for a bad unit or if the clock did not
 * come up. */
bool     dac_triangle_start(uint8_t unit, uint16_t low, uint16_t high, uint16_t slpdat);
/* One DAC off; the shared clock stays on while the other one runs. */
void     dac_off(uint8_t unit);
void     dac_all_off(void);

bool     dac_running(uint8_t unit);
uint16_t dac_low(uint8_t unit);
uint16_t dac_high(uint8_t unit);
uint16_t dac_slpdat(uint8_t unit);
uint32_t dac_period_ns(uint8_t unit);  /* of the triangle, from its settings */
/* 1 or 2 if exactly one DAC runs, 2 if both do (the boot test's own), 0 if
 * none - what dactest.c judges its samples against. */
uint8_t  dac_active(void);
/* "RA1" / "RA8": the pin that DAC's output buffer drives. */
const char *dac_pin_name(uint8_t unit);

/* Put DAC2 on the chip's internal UREF line (UREFCON.INSEL = 7), which
 * every ADC core can sample as its AN7 input (Table 16-2). This is how
 * the DAC test reaches the ADC: no pin, no wire, no core switch. Pass
 * true to additionally drive UREF onto its external pin - the internal
 * path should not need it. False if the register did not take. */
bool     uref_route_dac2(bool drive_pin);
void     uref_off(void);
uint32_t uref_insel(void);

void     dac_regs_dump(void);

#endif /* DAC_H */

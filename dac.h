/*
 * dac.h - DAC2 as a signal source for the ADC test (dac.c)
 *
 * DAC2 is one of the two DACs with an output buffer to a pin: DACOUT2 =
 * RA8 (128-pin device pin 7; DIM pin P44 = capacitive touch pad 2 on the
 * EV74H48A). RA8 is also AD5AN3, so ADC core 5 can measure the DAC on the
 * same pin without a wire. Triangle Wave mode (DS70005591D 18.4.x,
 * Example 18-3): the hardware ramps between DACLOW and DACDAT at
 * SLPDAT/16 codes per DAC step, no CPU involved. Slope duration per
 * Equation 18-4: T = (DACDAT - DACLOW) * 16 * T_DAC / SLPDAT with T_DAC =
 * 2 / F_DAC; a period is two slopes. DAC clock = CLKGEN7 (clock.c), the
 * PLL1 VCO divider at 400 MHz.
 */
#ifndef DAC_H
#define DAC_H

#include <stdbool.h>
#include <stdint.h>

/* The DAC's usable code range (18.4.2, p1417: "bound between 0x0CD and
 * 0xF32", 5 % to 95 % of VDD). */
#define DAC_CODE_MIN   0x0CDu
#define DAC_CODE_MAX   0xF32u

/* Triangle between the DAC codes low and high, SLPDAT in 12.4 fixed
 * point (SLPDAT/16 codes per DAC step). Refused (false) unless low >=
 * 0xCD + SLPDAT and high <= 0xF32 - SLPDAT (note 1 of Example 18-3,
 * p1422), or if the clock does not come up. Enables CLKGEN7, the DAC and
 * the pin buffer; every data write is taken at once (UPDTRG = 11). */
bool     dac2_triangle_start(uint16_t low, uint16_t high, uint16_t slpdat);
/* A static level, DAC_CODE_MIN..MAX, same enables. dac2_set() then
 * changes it with one register write - fast enough for an interrupt at
 * 100 kHz; the CPU-stepped signal of the chain test's low-rate stages. */
bool     dac2_level_start(uint16_t code);
void     dac2_set(uint16_t code);
void     dac2_off(void);
bool     dac2_running(void);
bool     dac2_triangle(void);          /* running, in Triangle Wave mode */
uint16_t dac2_low(void);
uint16_t dac2_high(void);
uint16_t dac2_slpdat(void);
/* The triangle's period (both slopes) in ns, and the number of samples
 * one slope lasts at a given sample rate, times 1000 - both from the
 * settings and the DAC clock as read back (Equation 18-4, p1420). 0 when
 * no triangle runs. */
uint32_t dac2_period_ns(void);
uint32_t dac2_slope_samples_x1000(uint32_t sample_hz);

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

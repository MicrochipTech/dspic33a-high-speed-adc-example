/*
 * dac.h - DAC1 and DAC2 as signal sources for the ADC test (dac.c)
 *
 * The device has eight 12-bit PDM DACs, one per analog comparator, and two
 * of them have an output buffer to a pin: DACOUT1 = RA1 and DACOUT2 = RA8
 * (128-pin device pins 7 and 14, 64-pin pins 3 and 7). Both pins are also
 * ADC inputs of core 5 - RA1 is AD5AN1, RA8 is AD5AN3 - so core 5 can read
 * either DAC back on its own pin, with no wire. Any other ADC input needs
 * one: the GUI's board tile shows which two pads to connect. The chain
 * test uses the DAC2/RA8/AD5AN3 route specifically (ANALYSIS.md C.11).
 *
 * Triangle Wave mode (DS70005591D 18.4.x, Example 18-3): the hardware
 * ramps between DACLOW and DACDAT at SLPDAT/16 codes per DAC step, no CPU
 * involved. Slope duration per Equation 18-4: T = (DACDAT - DACLOW) * 16 *
 * T_DAC / SLPDAT with T_DAC = 2 / F_DAC; a period is two slopes. DAC clock
 * = CLKGEN7 (clock.c), the PLL1 VCO divider at 400 MHz.
 */
#ifndef DAC_H
#define DAC_H

#include <stdbool.h>
#include <stdint.h>

#define DAC_UNITS   2                  /* the two with an output pin      */

/* The DAC's usable code range (18.4.2, p1417: "bound between 0x0CD and
 * 0xF32", 5 % to 95 % of VDD). Applies to both units. */
#define DAC_CODE_MIN   0x0CDu
#define DAC_CODE_MAX   0xF32u

/* Triangle between the DAC codes low and high, SLPDAT in 12.4 fixed point
 * (SLPDAT/16 codes per DAC step). `unit` is 1 or 2. Refused (false) for a
 * bad unit, unless low >= DAC_CODE_MIN + slpdat and high <= DAC_CODE_MAX -
 * slpdat (note 1 of Example 18-3, p1422), or if the clock does not come
 * up. Enables CLKGEN7, the DAC and its pin buffer; every data write is
 * taken at once (UPDTRG = 11 binary = 3, p1409). */
bool     dac_triangle_start(uint8_t unit, uint16_t low, uint16_t high, uint16_t slpdat);
/* A static level, DAC_CODE_MIN..MAX, same enables. dac_set() then changes
 * it with one register write - fast enough for an interrupt at 100 kHz;
 * the CPU-stepped signal of the chain test's low-rate stages. */
bool     dac_level_start(uint8_t unit, uint16_t code);
void     dac_set(uint8_t unit, uint16_t code);
/* One DAC off; the shared clock (CLKGEN7) stays on while the other runs. */
void     dac_off(uint8_t unit);
void     dac_all_off(void);

bool     dac_running(uint8_t unit);
bool     dac_triangle(uint8_t unit);   /* running, in Triangle Wave mode  */
uint16_t dac_low(uint8_t unit);
uint16_t dac_high(uint8_t unit);
uint16_t dac_slpdat(uint8_t unit);
/* The triangle's period (both slopes) in ns, and the number of samples
 * one slope lasts at a given sample rate, times 1000 - both from the
 * settings and the DAC clock as read back (Equation 18-4, p1420). 0 when
 * no triangle runs. */
uint32_t dac_period_ns(uint8_t unit);
uint32_t dac_slope_samples_x1000(uint8_t unit, uint32_t sample_hz);
/* 1 or 2 if exactly one DAC runs, 2 if both do (the boot test's own), 0 if
 * none - what dactest.c judges its samples against. */
uint8_t  dac_active(void);
/* "RA1" / "RA8": the pin that DAC's output buffer drives. */
const char *dac_pin_name(uint8_t unit);

/* DAC2-only names, for the chain test's fixed pin route (DACOUT2 = RA8 =
 * AD5AN3) and its low-latency ISR path: same behaviour as the generic
 * calls above with unit = 2, kept as their own names because that is what
 * chaintest.c spells (dac2_set() in particular is called from an
 * interrupt at up to 100 kHz and must stay a single register write). */
bool     dac2_triangle_start(uint16_t low, uint16_t high, uint16_t slpdat);
bool     dac2_level_start(uint16_t code);
void     dac2_set(uint16_t code);
void     dac2_off(void);
bool     dac2_running(void);
bool     dac2_triangle(void);
uint16_t dac2_low(void);
uint16_t dac2_high(void);
uint16_t dac2_slpdat(void);
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

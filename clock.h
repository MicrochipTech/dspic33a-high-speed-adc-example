/*
 * clock.h - clock tree of the ADC/DMA example (clock.c)
 */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>
#include <stdbool.h>

/* FRC -> PLL1 320 MHz -> CLKGEN6 (ADC), PLL2 200 MHz -> CLKGEN1 (system).
 * Every wait is bounded; a step that fails stops in fail(1..4). Enables
 * the clock-fail interrupt (_CLKFInterrupt in clock.c, stops in fail(10)). */
void clock_init(void);

/* True once CLKGEN1 runs on PLL2 (200 MHz); false on the FRC or the
 * backup FRC (8 MHz). Everything that has to time itself or pick a baud
 * divider asks this instead of reading CLK1CON. */
bool     clock_cpu_on_pll(void);
uint32_t clock_cpu_hz(void);

/* ADC clock divider, CLKGEN6 - the only thing that sets the sample rate
 * in this example, because the conversions run back-to-back and nothing
 * else on this silicon paces them (docs/HARDWARE-LOG.md runs 4 to 7).
 *
 * The ratio is given in hundredths: 100 = 320 MHz straight through,
 * 200 = /2, 250 = /2.5, 1000 = /10. The divided clock is
 * Fin / (2 * (INTDIV + FRACDIV/512)) (DS70005591D Example 12-2, CLKnDIV
 * description), so INTDIV = ratio/2 and FRACDIV carries the rest in
 * 1/512 steps; 32 MHz is the ADC's minimum (Table 16-1, p1223), so 1000
 * is the largest ratio and 4 MSPS the slowest rate. Fractional ratios
 * are deliberately part of the sweep: a rate that lands exactly between
 * its two integer neighbours is the proof that FRACDIV works at all.
 *
 * Sequence, and the ADC core and the DMA channel must be down before it
 * (capture.c does that and brings them back the way it does at boot):
 * generator off, divider written and read back, generator on, DIVSWEN
 * awaited, CLKRDY awaited, fields read back again. Every wait bounded.
 * Returns CLKDIV_OK or the step that failed - which is the difference
 * run 7 could not see. clock_adc_div_error() puts it into words. */
#define CLKDIV_OK           0u
#define CLKDIV_RANGE        1u
#define CLKDIV_NOT_WRITTEN  2u
#define CLKDIV_DIVSWEN      3u
#define CLKDIV_CLKRDY       4u
#define CLKDIV_LOST         5u
#define CLKDIV_ADC          6u   /* capture.c: core did not come back */
#define CLKDIV_INTDIV0      7u   /* ratio 1 < r < 2: FRACDIV alone does nothing */
uint32_t    clock_adc_set_div(uint32_t ratio_h);
const char *clock_adc_div_error(uint32_t rc);
/* CLKGEN6 off (the ADC has no clock then; the core must be off first) and
 * on again with the divider it had, CLKRDY awaited, bounded. */
void     clock_adc_off(void);
bool     clock_adc_on(void);
uint32_t clock_adc_div(void);
uint32_t clock_adc_hz(void);

/* CLKGEN7 = DAC clock, from PLL1 (320 MHz; the datasheet's "400 MHz
 * typical" is the design point, the range is not specified). On with
 * OSWEN and CLKRDY awaited, bounded; off. clock_dac_hz() is what it runs
 * at, for the triangle-wave period. */
bool     clock_dac_on(void);
void     clock_dac_off(void);
uint32_t clock_dac_hz(void);

/* The clock registers as "name: 0x........" lines (part of regs_dump()). */
void clock_regs_dump(void);

#endif /* CLOCK_H */

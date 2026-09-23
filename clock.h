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

/* ADC clock divider, CLKGEN6. The ratio is given in hundredths: 100 =
 * 320 MHz straight through (INTDIV 0), 200 = /2, 250 = /2.5, 1000 = /10.
 * The divided clock is Fin / (2 * (INTDIV + FRACDIV/512)) (DS70005591D
 * Example 12-2, CLKnDIV description), so INTDIV = ratio/2 and FRACDIV
 * carries the rest in 1/512 steps; 32 MHz is the ADC's minimum (Table
 * 16-1, p1223), so 1000 is the largest ratio. Ratios between 100 and 200
 * mean INTDIV = 0 with a fraction - whether the hardware divides then or
 * bypasses is for the rate test to say. Repeats the generator's boot
 * sequence: off, divider, on, DIVSWEN, CLKRDY, every wait bounded. False
 * for a bad ratio or a wait that ran out; the divider is then whatever
 * the hardware says (clock_adc_div(), read back in hundredths). The ADC
 * core must be OFF while this runs: capture.c takes it down, calls this,
 * brings it back. */
bool     clock_adc_set_div(uint32_t ratio_h);
uint32_t clock_adc_div(void);
uint32_t clock_adc_hz(void);

/* The clock registers as "name: 0x........" lines (part of regs_dump()). */
void clock_regs_dump(void);

#endif /* CLOCK_H */

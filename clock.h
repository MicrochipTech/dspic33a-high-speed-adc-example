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

/* ADC clock divider, CLKGEN6. ratio 1 = 320 MHz straight through
 * (INTDIV 0), or an even 2..10: the divided clock is Fin / (2 * INTDIV)
 * (DS70005591D, CLKnDIV INTDIV description), and 32 MHz is the ADC's
 * minimum (Table 16-1, p1223), so 10 is the largest ratio. Repeats the
 * generator's boot sequence: off, divider, on, DIVSWEN, CLKRDY, every
 * wait bounded. False for a bad ratio or a wait that ran out; the divider
 * is then whatever the hardware says (clock_adc_div()). The ADC core must
 * be OFF while this runs: capture.c takes it down, calls this, brings it
 * back. */
bool     clock_adc_set_div(uint32_t ratio);
uint32_t clock_adc_div(void);
uint32_t clock_adc_hz(void);

/* The clock registers as "name: 0x........" lines (part of regs_dump()). */
void clock_regs_dump(void);

#endif /* CLOCK_H */

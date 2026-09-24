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
#define CLKDIV_FOUTSWEN     8u   /* PLL1 output divider switch never completed  */
#define CLKDIV_PLLRDY       9u   /* PLL1 never locked again                     */
uint32_t    clock_adc_set_div(uint32_t ratio_h);
const char *clock_adc_div_error(uint32_t rc);
/* CLKGEN6 off (the ADC has no clock then; the core must be off first) and
 * on again with the divider it had, CLKRDY awaited, bounded. */
/* The ADC clock the other way round: PLL1's two output dividers.
 *
 * PLL1 feeds nothing but the ADC path (the CPU runs off PLL2), so it can
 * be retuned without touching anything else. FVCO is 1600 MHz and the
 * output is FVCO / (POSTDIV1 * POSTDIV2), both fields 1..7 and POSTDIV1
 * >= POSTDIV2 (p778). That gives 320 down to 32.65 MHz, i.e. 40 down to
 * 4.08 MSPS, and 5/5 = 64 MHz = 8 MSPS exactly.
 *
 * This exists because the CLKGEN6 divider does not work: through runs 8
 * and 9 every ratio was written, read back and confirmed by DIVSWEN and
 * CLKRDY, with the generator off and with it running, and the ADC kept
 * converting at 40 MSPS at every one of them (docs/HARDWARE-LOG.md). The
 * PLL's own switching sequence, in contrast, is the one clock_init()
 * performs at every boot - if it did not work the board would not come
 * up at all.
 *
 * "The output dividers POSTDIV1 and POSTDIV2 should not be changed while
 * the PLL is operating" (p778), so the caller takes the ADC core down
 * first, exactly as for the divider. Sequence: write PLL1DIV, FOUTSWEN,
 * wait for it to clear, wait for PLL1RDY, wait for CLKGEN6's CLKRDY.
 * Returns CLKDIV_OK or the step that failed. */
uint32_t clock_adc_set_pll(uint32_t postdiv1, uint32_t postdiv2);
uint32_t clock_adc_pll_postdiv1(void);
uint32_t clock_adc_pll_postdiv2(void);

void     clock_adc_off(void);
bool     clock_adc_on(void);
uint32_t clock_adc_div(void);
/* The ADC input clock as the registers actually say it is: the FRC
 * through PLL1 (PLLPRE, PLLFBDIV, POSTDIV1, POSTDIV2) and then the
 * CLKGEN6 divider. Nothing is assumed - if a switch did not take, this
 * number still reports what the hardware holds. */
uint32_t clock_adc_hz(void);

/* CLKGEN13 = the clock of the ADC's trigger module (SCCP1).
 *
 * It exists as its own entry point because of one sentence from a
 * Microchip support case: "ADC triggers go through synchronizers. If the
 * trigger source is clocked from a different clock source than the ADC,
 * trigger timing can be jittery. To avoid this the ADC trigger source
 * module must be clocked from the same clock source used for ADC." The
 * same case describes a working setup in which CLKGEN6 and CLKGEN13 are
 * both sourced from PLL1 - which is exactly what clock_trig_on() sets up.
 *
 * On with OSWEN and CLKRDY awaited, bounded; off. clock_trig_hz() is what
 * it runs at, read back from the registers. */
bool     clock_trig_on(void);
void     clock_trig_off(void);
uint32_t clock_trig_hz(void);

/* The standard-speed peripheral clock, which is what the SCCP uses when
 * it is NOT put on CLKGEN13 - it comes from CLKGEN1 and therefore PLL2,
 * a different source from the ADC's. */
uint32_t clock_periph_hz(void);

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

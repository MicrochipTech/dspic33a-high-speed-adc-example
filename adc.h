/*
 * adc.h - the ADC core of the ADC/DMA example (adc.c)
 */
#ifndef ADC_H
#define ADC_H

#include <stdint.h>
#include "board.h"

/* ------------------------------------------------------------------ *
 * ADC core selection
 *
 * The five ADC cores have identical register sets, only the prefix
 * differs (AD1..., AD5...). ADCREG(x) expands to the register of the core
 * selected by ADC_INSTANCE in board.h, e.g. ADCREG(CH0CON1bits).
 * The DMA trigger code follows from the ATDF value-group DMA_SEL__CHSEL:
 * "ADCn Done CH0" = 0x2F, 0x35, 0x3B, 0x41, 0x48 for n = 1..5.
 * ------------------------------------------------------------------ */
#define ADC_CAT_(a, b, c)  a##b##c
#define ADC_CAT(a, b, c)   ADC_CAT_(a, b, c)
#define ADCREG(suffix)     ADC_CAT(AD, ADC_INSTANCE, suffix)

/* ADC_CH0_IRQ is the interrupt number of "ADCn data channel 0 done"
 * (ATDF interrupt list: AD1CH0 157, AD2CH0 179, AD3CH0 201, AD4CH0 221,
 * AD5CH0 241); the core's twelve channel/comparator events follow it
 * (CH0, CMP0, CH1, CMP1 ... CH5, CMP5 = IRQ .. IRQ + 11). The enable and
 * flag bits sit in IEC[IRQ / 32] bit IRQ % 32 - for ADC3 that is IEC6
 * bit 9, for ADC1 IEC4 bit 29 (device header _IEC4_AD1CH0IE_MASK). */
#if   ADC_INSTANCE == 1
#define DMA_TRIG_ADC_CH0   0x2Fu
#define ADC_CH0_IRQ        157u
#elif ADC_INSTANCE == 2
#define DMA_TRIG_ADC_CH0   0x35u
#define ADC_CH0_IRQ        179u
#elif ADC_INSTANCE == 3
#define DMA_TRIG_ADC_CH0   0x3Bu
#define ADC_CH0_IRQ        201u
#elif ADC_INSTANCE == 4
#define DMA_TRIG_ADC_CH0   0x41u
#define ADC_CH0_IRQ        221u
#elif ADC_INSTANCE == 5
#define DMA_TRIG_ADC_CH0   0x48u
#define ADC_CH0_IRQ        241u
#else
#error "ADC_INSTANCE must be 1..5"
#endif
#define ADC_IRQ_COUNT      12u      /* CH0..CH5 and CMP0..CMP5 of the core */

/* The core's channel-0 event as the CPU sees it (IFS bit), for the
 * status line: 1 while the DMA runs means the event is visible to the
 * CPU although its enable is clear. */
uint32_t adc_ch0_irq_flag(void);

/* ADC core ADC_INSTANCE, channel 0, Integration mode, CNT = SAMPLES_PER_BUF,
 * conversions inside a burst paced by the ADC's repeat timer with period
 * `rptcnt` TAD (TAD = 12.5 ns at 320 MHz: 2 = 40 MSPS, 63 = 1.27 MSPS).
 * Stops in fail(5) if the core never reports ready. */
void adc_init(uint8_t pinsel, uint8_t samc, uint8_t rptcnt);

/* Trigger one burst of SAMPLES_PER_BUF conversions. */
void adc_start_burst(void);

/* Input pin (PINSEL 0..15) and sample time (SAMC 0..31) of channel 0.
 * Written straight to the register: only while no burst is running. */
void    adc_set_input(uint8_t pinsel, uint8_t samc);
uint8_t adc_pinsel(void);
uint8_t adc_samc(void);

/* Repeat-timer period (RPTCNT, 2..63 TAD). Same rule as adc_set_input(). */
void    adc_set_period(uint8_t rptcnt);
uint8_t adc_period(void);

/* What re-triggers the conversions inside a burst: TRG2SRC, DS70005591D
 * Table 16-4 (p1227). Same rule: between bursts, through capture.c. */
#define ADC_TRG2_B2B      2u        /* back-to-back: as fast as it goes  */
#define ADC_TRG2_REPEAT   3u        /* the ADC's repeat timer, RPTCNT    */
#define ADC_TRG2_SCCP1    32u       /* SCCP1 timer period match          */
void    adc_set_trg2(uint8_t trg2src);
uint8_t adc_trg2(void);

/* The core's registers as "name: 0x........" lines (part of regs_dump()). */
void adc_regs_dump(void);

#endif /* ADC_H */

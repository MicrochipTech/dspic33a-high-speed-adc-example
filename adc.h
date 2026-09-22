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

#if   ADC_INSTANCE == 1
#define DMA_TRIG_ADC_CH0   0x2Fu
#elif ADC_INSTANCE == 2
#define DMA_TRIG_ADC_CH0   0x35u
#elif ADC_INSTANCE == 3
#define DMA_TRIG_ADC_CH0   0x3Bu
#elif ADC_INSTANCE == 4
#define DMA_TRIG_ADC_CH0   0x41u
#elif ADC_INSTANCE == 5
#define DMA_TRIG_ADC_CH0   0x48u
#else
#error "ADC_INSTANCE must be 1..5"
#endif

/* ADC core ADC_INSTANCE, channel 0, Integration mode, CNT = SAMPLES_PER_BUF.
 * Stops in fail(5) if the core never reports ready. */
void adc_init(uint8_t pinsel, uint8_t samc);

/* Trigger one burst of SAMPLES_PER_BUF conversions. */
void adc_start_burst(void);

/* Input pin (PINSEL 0..15) and sample time (SAMC 0..31) of channel 0.
 * Written straight to the register: only while no burst is running. */
void    adc_set_input(uint8_t pinsel, uint8_t samc);
uint8_t adc_pinsel(void);
uint8_t adc_samc(void);

#endif /* ADC_H */

/*
 * adc.h - the ADC core of the ADC/DMA example (adc.c)
 */
#ifndef ADC_H
#define ADC_H

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

/* ------------------------------------------------------------------ *
 * ADC core selection - at run time
 * The five ADC cores have identical register sets, only the prefix
 * differs (AD1..., AD5...). adc_cur points at the table row of the
 * active core: ADCREG(x) is that core's register x as a 32-bit word,
 * ADCBITS(x) the same register through the AD3...BITS bit-field type
 * (identical layout on every core). ADC_INSTANCE in board.h is the core
 * the boot starts on; adc_select() switches, with the core down.
 * The DMA trigger code follows from the ATDF value-group DMA_SEL__CHSEL:
 * "ADCn Done CH0" = 0x2F, 0x35, 0x3B, 0x41, 0x48 for n = 1..5. The CH0
 * interrupt of core n is IRQ 157/179/201/221/241: word IRQ/32, bit
 * IRQ%32 = IEC4.29, IEC5.19, IEC6.9, IEC6.29, IEC7.17 (device header,
 * _IECx_ADnCH0IE_POSITION).
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t  core;
    volatile uint32_t *CON, *STAT, *SWTRG, *CH0CON1, *CH0CNT, *CH0RES, *CH0DATA;
    volatile uint32_t *IEC, *IFS;     /* the words holding this core's IRQs */
    uint32_t ch0_mask;                /* CH0 IRQ bit in those words        */
    uint8_t  dma_trigger;             /* DMA_SEL CHSEL "ADCn Done CH0"     */
} adc_core_t;
extern const adc_core_t *adc_cur;
#define ADCREG(r)   (*adc_cur->r)
#define ADCBITS(r)  (*(volatile AD3##r##BITS *)adc_cur->r)

#if (ADC_INSTANCE < 1) || (ADC_INSTANCE > 5)
#error "ADC_INSTANCE must be 1..5"
#endif

/* Switch the active core (1..5). Only with the core down (adc_deinit)
 * and the DMA idle; the caller re-runs adc_init() and re-arms the DMA
 * (capture_select_core() does all of it). False for a bad number. */
bool     adc_select(uint8_t core);
uint8_t  adc_core(void);
uint8_t  adc_dma_trigger(void);              /* DMA_SEL code of the active core */
const volatile void *adc_dma_source(void);   /* &ADxCH0RES of the active core   */
bool     adc_ch0_flag(void);                 /* its CH0 interrupt flag          */

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
#define ADC_TRG2_SCCP1    34u       /* SCCP1 trigger: 100010 in Tables 16-3
                                     * AND 16-4 (p1226 f.). 32 = 100000 is
                                     * "PTG trigger 12" - what runs 5 and 6
                                     * on the board were really testing. */
/* Not TRG2SRC values - pacing sources capture.c owns:
 *   64  the ADC clock divider (clock.c), TRG2SRC = back-to-back
 *   65  one conversion per SCCP1 trigger: Single Conversion mode with the
 *       SCCP1 trigger as TRG1SRC - what Microchip's own 40 MSPS example
 *       does (8 channels x 5 MSPS, MCC: "Single Sample", trigger source
 *       "SCCP1 Trigger Event"). No burst, no CNT, no restart. */
#define ADC_PACE_CLKDIV   64u
#define ADC_PACE_SINGLE   65u
void    adc_set_trg2(uint8_t trg2src);
uint8_t adc_trg2(void);

/* Take the core down and bring it back. The clock of a running core is
 * not changed: the caller calls adc_deinit(), changes CLKGEN6, calls
 * adc_reinit() and gets ADRDY back (bounded wait; false on timeout, and
 * the core is then not usable). adc_deinit() also clears the channel's
 * event flags and a stale CH0RDY, so nothing old triggers the DMA when
 * the core comes back. Only while no burst is in flight. The channel
 * configuration (adc_init) survives ON = 0 and is not repeated. */
void    adc_deinit(void);
bool    adc_reinit(void);
bool    adc_ready(void);

/* Channel 0 operating mode. Burst: Integration mode, software trigger
 * starts CNT conversions (the boot configuration). Single: one conversion
 * per TRG1 trigger from the given source, TRG2 unused. Only while no
 * burst is in flight and no trigger is running. */
void    adc_set_mode_burst(void);
void    adc_set_mode_single(uint8_t trg1src);

/* The core's registers as "name: 0x........" lines (part of regs_dump()). */
void adc_regs_dump(void);

#endif /* ADC_H */

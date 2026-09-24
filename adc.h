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
    volatile uint32_t *IEC, *IFS;     /* the words holding this core's CH0 IRQ */
    uint32_t ch0_mask;                /* CH0 IRQ bit in those words        */
    uint32_t ch0_irq;                 /* CH0 IRQ number (ATDF interrupt list) */
    uint8_t  dma_trigger;             /* DMA_SEL CHSEL "ADCn Done CH0"     */
} adc_core_t;
extern const adc_core_t *adc_cur;
#define ADCREG(r)   (*adc_cur->r)
#define ADCBITS(r)  (*(volatile AD3##r##BITS *)adc_cur->r)

#if (ADC_INSTANCE < 1) || (ADC_INSTANCE > 5)
#error "ADC_INSTANCE must be 1..5"
#endif
/* The twelve channel/comparator events of a core follow its CH0 IRQ
 * (CH0, CMP0, CH1, CMP1 ... CH5, CMP5 = ch0_irq .. ch0_irq + 11, ATDF
 * interrupt list). ADC1's and ADC4's ranges cross a 32-bit IEC/IFS word
 * boundary (157..168, 221..232), so code that walks the range indexes
 * IEC[irq / 32] per iteration instead of touching one word. */
#define ADC_IRQ_COUNT      12u      /* CH0..CH5 and CMP0..CMP5 of the core */

/* Switch the active core (1..5). Only with the core down (adc_deinit)
 * and the DMA idle; the caller re-runs adc_init() and re-arms the DMA
 * (capture_select_core() does all of it). False for a bad number. */
bool     adc_select(uint8_t core);
uint8_t  adc_core(void);
uint8_t  adc_dma_trigger(void);              /* DMA_SEL code of the active core */
const volatile void *adc_dma_source(void);   /* &ADxCH0RES of the active core   */
bool     adc_ch0_flag(void);                 /* its CH0 interrupt flag          */
/* Leftovers of a stopped stream: CH0RDY (cleared by reading CH0DATA) and
 * the core's event flags. Between tests, with the core idle. */
void     adc_clear_events(void);

/* ADC core ADC_INSTANCE, channel 0, Integration mode, CNT = the DMA block
 * length, conversions back-to-back (TRG2SRC = 2) - the only mechanism
 * this silicon honours. The rate comes from the ADC clock alone, see
 * clock_adc_set_div(). Stops in fail(5) if the core never reports ready. */
void adc_init(uint8_t pinsel, uint8_t samc);

/* Trigger one burst of CNT conversions. */
void adc_start_burst(void);

/* Input pin (PINSEL 0..15) and sample time (SAMC 0..31) of channel 0.
 * Written straight to the register: only while no burst is running. */
void    adc_set_input(uint8_t pinsel, uint8_t samc);
uint8_t adc_pinsel(void);
uint8_t adc_samc(void);

/* ---- Channel mode and trigger, for the variant matrix ----
 * Only while no burst is in flight; capture.c takes the stream down
 * first. Trigger codes come from the ATDF, which names what each number
 * selects - see sccp.h for why that matters here.
 *   burst        Integration, software start, back-to-back inside
 *   single       one conversion per TRG1 trigger, no burst
 *   oversample   Integration of ACCNUM conversions per ready event */
void    adc_set_mode_burst(void);
void    adc_set_mode_single(uint8_t trg1src);
void    adc_set_mode_oversample(uint8_t accnum);
void    adc_set_trg2(uint8_t trg2src);
void    adc_set_period(uint8_t rptcnt);    /* RPTCNT, the repeat timer */
uint8_t adc_mode(void);
uint8_t adc_trg1(void);
uint8_t adc_trg2(void);
uint8_t adc_accnum(void);
uint8_t adc_period(void);

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
/* Conversions per burst (CNT, up to 65535): the DMA block length. Set
 * by capture_init() before every start, idle only. */
void    adc_set_burst_len(uint32_t count);

/* The core's registers as "name: 0x........" lines (part of regs_dump()). */
void adc_regs_dump(void);

#endif /* ADC_H */

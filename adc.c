/*
 * adc.c
 *
 * The ADC core of the ADC/DMA example: one channel in Integration mode,
 * CNT conversions per burst, started by software, kept going by the
 * back-to-back trigger. Which core (AD1..AD5) is ADC_INSTANCE in
 * board.h. The DMA side and the burst restart live in capture.c.
 */

#include <xc.h>
#include <stdbool.h>
#include "board.h"
#include "adc.h"
#include "capture.h"
#include "console.h"

/* One row per core: its registers, its interrupt words and CH0 bit, its
 * DMA trigger code (adc.h explains the numbers). Address constants, so
 * the table lives in flash. */
#define ADC_ROW(n, iec, ifs, bit, trig) \
    { (n), &AD##n##CON, &AD##n##STAT, &AD##n##SWTRG, &AD##n##CH0CON1, \
      &AD##n##CH0CNT, &AD##n##CH0RES, &AD##n##CH0DATA, &iec, &ifs, 1ul << (bit), (trig) }
static const adc_core_t adc_cores[5] = {
    ADC_ROW(1, IEC4, IFS4, 29, 0x2Fu),
    ADC_ROW(2, IEC5, IFS5, 19, 0x35u),
    ADC_ROW(3, IEC6, IFS6,  9, 0x3Bu),
    ADC_ROW(4, IEC6, IFS6, 29, 0x41u),
    ADC_ROW(5, IEC7, IFS7, 17, 0x48u),
};
const adc_core_t *adc_cur = &adc_cores[ADC_INSTANCE - 1];

bool adc_select(uint8_t core)
{
    if ((core < 1u) || (core > 5u)) { return false; }
    adc_cur = &adc_cores[core - 1u];
    return true;
}

uint8_t adc_core(void)                    { return adc_cur->core; }
uint8_t adc_dma_trigger(void)             { return adc_cur->dma_trigger; }
const volatile void *adc_dma_source(void) { return adc_cur->CH0RES; }
bool adc_ch0_flag(void)                   { return (*adc_cur->IFS & adc_cur->ch0_mask) != 0u; }

void adc_clear_events(void)
{
    (void)ADCREG(CH0DATA);
    *adc_cur->IFS = 0u;
}
#include "diag.h"

/* ------------------------------------------------------------------ *
 * ADC setup - one channel, Integration mode, the ADC's repeat timer
 * pacing the conversions inside a burst
 *
 * DS70005591D 16.4.4 (p1321) and 16.4.5 (p1322):
 *   - MODE = 10 (Integration): CNT conversions per burst, "the first
 *     conversion is initiated by a trigger selected by TRG1SRC and all
 *     subsequent conversions are executed by a trigger selected by
 *     TRG2SRC".
 *   - TRG1SRC = 000001: software trigger, ADnSWTRG (Table 16-3, p1226).
 *   - TRG2SRC = 000011: "Conversion repeat timer trigger defined by
 *     RPTCNT[5:0] (ADnCON[23:18]) bits" (Table 16-4, p1227). p1322: "The
 *     channel is triggered from an internal ADC repeat timer ... This
 *     timer is clocked from the ADC analog core clock (TAD), and its
 *     period is set by RPTCNT[5:0] bits." Datasheet Example 16-4 (p1330)
 *     uses exactly this, with RPTCNT = 60 ("63 is maximum").
 *     THIS IS WHAT MAKES THE SAMPLE RATE DETERMINISTIC. The first board
 *     runs used 000010, back-to-back ("re-triggered immediately after
 *     the previous conversion is finished", and p1322 adds: "The timing
 *     is affected (can be delayed) by priorities of other channels"),
 *     and the sweep showed that the delivered rate did not follow SAMC
 *     at all - the ADC simply ran as fast as it could. With the repeat
 *     timer the rate is TAD-exact: period = RPTCNT x TAD (whether the
 *     hardware counts RPTCNT or RPTCNT + 1 cycles is what the measured
 *     rate in the sweep table settles), TAD = 12.5 ns at 320 MHz, so
 *     RPTCNT 2 = 40 MSPS and RPTCNT 63 = 1.27 MSPS. Slower than that
 *     needs a divided ADC clock (CLK6DIV) or a CCP timer as trigger
 *     (TRG2SRC = 32 = CCP1 in Example 16-8, p1334).
 *   - TRG2SRC "are not used for a Single Conversion mode" (p1322), and
 *     000010/000011 are reserved for TRG1SRC - so MODE = 00 cannot
 *     free-run.
 *   - IRQSEL = 0: "the channel interrupt is generated after each single
 *     conversion when result is ready in ADxRESn" (p1266). That per-
 *     conversion event is what triggers the DMA. IRQSEL = 1 would fire
 *     only once per burst.
 *   - EIEN = 0: note 4 on p1265, no early interrupt with DMA transfers.
 *   - The per-conversion result is ADxCH0RES[11:0]; ADxCH0DATA is the
 *     accumulator of the burst (p1270) and is not what we want.
 *
 * Microchip's own 40 MSPS example for this board (dspic33ak-curiosity-
 * adc-40msps) uses MODE = 2, CNT = 800, TRG1SRC = 1, TRG2SRC = 2 (back-
 * to-back) - and no DMA: it copies AD3CH0RES with a hand-timed assembly
 * loop, "200MHz CPU : 40MSPS = 5 instructions per sample", for 800
 * samples. That is the budget one sample has at 40 MSPS.
 * ------------------------------------------------------------------ */
void adc_init(uint8_t pinsel, uint8_t samc, uint8_t rptcnt)
{
    ADCBITS(CON).ON = 0;

    /* The repeat timer's period, RPTCNT in ADxCON[23:18], in TAD. The
     * reset value seen on the board was 18 (AD3CON = 0xC34A8000). */
    ADCBITS(CON).RPTCNT = rptcnt;

    /* Channel 0 configuration, ADxCH0CON1 (DS70005591D p1265 f.) */
    ADCBITS(CH0CON1).PINSEL  = pinsel;      /* positive input select */
    ADCBITS(CH0CON1).NINSEL  = 0u;          /* negative input = AVSS */
    ADCBITS(CH0CON1).DIFF    = 0u;          /* single ended, unsigned*/
    ADCBITS(CH0CON1).FRAC    = 0u;          /* integer, right aligned*/
    ADCBITS(CH0CON1).SAMC    = samc;        /* sample time in TAD    */
    ADCBITS(CH0CON1).MODE    = 2u;          /* Integration           */
    ADCBITS(CH0CON1).ACCNUM  = 0u;          /* oversampling only     */
    ADCBITS(CH0CON1).IRQSEL  = 0u;          /* event per conversion  */
    ADCBITS(CH0CON1).EIEN    = 0u;          /* no early IRQ with DMA */
    ADCBITS(CH0CON1).TRG1SRC = 0x01u;       /* software trigger      */
    ADCBITS(CH0CON1).TRG2SRC = ADC_TRG2_REPEAT; /* capture.c may change it */

    /* Conversions per burst. One burst fills the whole DMA buffer, so
     * the DMA DONE interrupt is also the moment to start the next one.
     * CNT[15:0] in ADxCH0CNT (p1272), max 65535. */
    ADCREG(CH0CNT) = SAMPLES_PER_BUF;

    /* The channel-done event of this core is the DMA trigger. It must not
     * also reach the CPU: there is no handler for it, and with IRQSEL = 0
     * it would arrive on every conversion, 40 million times a second.
     * IEC6 bit 9 (AD3CH0IE, IRQ 201) resets to 0, but a previous program
     * or the debugger may have left it set, so clear ADC3's whole enable
     * word (IRQ 192..223) and its flags before the core starts. If the
     * trap report ever shows vector 201 with IEC6 = 0, the event reaches
     * the CPU regardless of the enable, and this assumption is wrong. */
    *adc_cur->IEC = 0u;
    *adc_cur->IFS = 0u;

    ADCBITS(CON).ON = 1;
    WAIT_WHILE(!ADCBITS(CON).ADRDY, 5u);    /* wait for the core     */
    console_trace("[adc] core ready, Integration mode, CNT 2048, repeat-timer trigger\r\n");
    console_trace_kv("[adc] pinsel", pinsel);
    console_trace_kv("[adc] samc", samc);
    console_trace_kv("[adc] rptcnt (period in TAD of 12.5 ns)", rptcnt);
}

/* Input pin and sample time of channel 0. Only safe while no burst is
 * running: capture.c applies a change between two bursts. */
void adc_set_input(uint8_t pinsel, uint8_t samc)
{
    ADCBITS(CH0CON1).PINSEL = pinsel;
    ADCBITS(CH0CON1).SAMC   = samc;
}

/* Period of the repeat timer, RPTCNT (2..63 TAD). Same rule: between
 * bursts, through capture.c. */
void adc_set_period(uint8_t rptcnt)
{
    ADCBITS(CON).RPTCNT = rptcnt;
}

uint8_t adc_period(void) { return (uint8_t)ADCBITS(CON).RPTCNT; }

void adc_set_trg2(uint8_t trg2src)
{
    ADCBITS(CH0CON1).TRG2SRC = trg2src;
}

uint8_t adc_trg2(void) { return (uint8_t)ADCBITS(CH0CON1).TRG2SRC; }

bool adc_ready(void) { return ADCBITS(CON).ADRDY != 0u; }

void adc_set_mode_burst(void)
{
    ADCBITS(CH0CON1).MODE    = 2u;          /* Integration           */
    ADCBITS(CH0CON1).TRG1SRC = 0x01u;       /* software trigger      */
}

void adc_set_mode_single(uint8_t trg1src)
{
    ADCBITS(CH0CON1).MODE    = 0u;          /* Single Conversion     */
    ADCBITS(CH0CON1).TRG1SRC = trg1src;     /* e.g. SCCP1 trigger    */
}

void adc_deinit(void)
{
    ADCBITS(CON).ON = 0;
    (void)ADCREG(CH0DATA);            /* clears a stale CH0RDY            */
    *adc_cur->IFS = 0u;               /* this core's event flags          */
}

bool adc_reinit(void)
{
    ADCBITS(CON).ON = 1;
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return true;                       /* no core to wait for              */
#else
    uint32_t n = WAIT_LIMIT;
    while (!ADCBITS(CON).ADRDY && (--n != 0u)) { }
    return n != 0u;
#endif
}

uint8_t adc_pinsel(void) { return (uint8_t)ADCBITS(CH0CON1).PINSEL; }
uint8_t adc_samc(void)   { return (uint8_t)ADCBITS(CH0CON1).SAMC; }

/* Start one burst of SAMPLES_PER_BUF conversions. Reading ADxCH0DATA
 * first clears CH0RDY from the previous burst, as datasheet Example 16-6
 * does before re-triggering. */
void adc_start_burst(void)
{
    (void)ADCREG(CH0DATA);
    ADCBITS(SWTRG).CH0TRG = 1u;
}

void adc_regs_dump(void)
{
    console_puts("[regs] adc\r\n");
    console_kv("core", adc_core());
    console_kv_hex("ADxCON", ADCREG(CON));
    console_kv_hex("ADxSTAT", ADCREG(STAT));
    console_kv_hex("ADxCH0CON1", ADCREG(CH0CON1));
    console_kv_hex("ADxCH0CNT", ADCREG(CH0CNT));
    console_kv_hex("ADxCH0RES", ADCREG(CH0RES));
    console_kv_hex("ADxCH0DATA", ADCREG(CH0DATA));
    console_kv_hex("IECx (this core's word)", *adc_cur->IEC);
    console_kv_hex("IFSx (this core's word)", *adc_cur->IFS);
    console_kv_hex("CH0 IRQ mask in it", adc_cur->ch0_mask);
}

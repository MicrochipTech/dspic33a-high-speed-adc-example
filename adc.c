/*
 * adc.c
 *
 * The ADC core of the ADC/DMA example: one channel in Integration mode,
 * CNT conversions per burst, started by software, kept going by the
 * back-to-back trigger. Which core (AD1..AD5) is ADC_INSTANCE in
 * board.h. The DMA side and the burst restart live in capture.c.
 */

#include <xc.h>
#include "board.h"
#include "adc.h"
#include "capture.h"
#include "console.h"
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
    ADCREG(CONbits).ON = 0;

    /* The repeat timer's period, RPTCNT in ADxCON[23:18], in TAD. The
     * reset value seen on the board was 18 (AD3CON = 0xC34A8000). */
    ADCREG(CONbits).RPTCNT = rptcnt;

    /* Channel 0 configuration, ADxCH0CON1 (DS70005591D p1265 f.) */
    ADCREG(CH0CON1bits).PINSEL  = pinsel;      /* positive input select */
    ADCREG(CH0CON1bits).NINSEL  = 0u;          /* negative input = AVSS */
    ADCREG(CH0CON1bits).DIFF    = 0u;          /* single ended, unsigned*/
    ADCREG(CH0CON1bits).FRAC    = 0u;          /* integer, right aligned*/
    ADCREG(CH0CON1bits).SAMC    = samc;        /* sample time in TAD    */
    ADCREG(CH0CON1bits).MODE    = 2u;          /* Integration           */
    ADCREG(CH0CON1bits).ACCNUM  = 0u;          /* oversampling only     */
    ADCREG(CH0CON1bits).IRQSEL  = 0u;          /* event per conversion  */
    ADCREG(CH0CON1bits).EIEN    = 0u;          /* no early IRQ with DMA */
    ADCREG(CH0CON1bits).TRG1SRC = 0x01u;       /* software trigger      */
    ADCREG(CH0CON1bits).TRG2SRC = 0x03u;       /* ADC repeat timer      */

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
    IEC6 = 0u;
    IFS6 = 0u;

    ADCREG(CONbits).ON = 1;
    WAIT_WHILE(!ADCREG(CONbits).ADRDY, 5u);    /* wait for the core     */
    console_trace("[adc] core ready, Integration mode, CNT 2048, repeat-timer trigger\r\n");
    console_trace_kv("[adc] pinsel", pinsel);
    console_trace_kv("[adc] samc", samc);
    console_trace_kv("[adc] rptcnt (period in TAD of 12.5 ns)", rptcnt);
}

/* Input pin and sample time of channel 0. Only safe while no burst is
 * running: capture.c applies a change between two bursts. */
void adc_set_input(uint8_t pinsel, uint8_t samc)
{
    ADCREG(CH0CON1bits).PINSEL = pinsel;
    ADCREG(CH0CON1bits).SAMC   = samc;
}

/* Period of the repeat timer, RPTCNT (2..63 TAD). Same rule: between
 * bursts, through capture.c. */
void adc_set_period(uint8_t rptcnt)
{
    ADCREG(CONbits).RPTCNT = rptcnt;
}

uint8_t adc_period(void) { return (uint8_t)ADCREG(CONbits).RPTCNT; }

uint8_t adc_pinsel(void) { return (uint8_t)ADCREG(CH0CON1bits).PINSEL; }
uint8_t adc_samc(void)   { return (uint8_t)ADCREG(CH0CON1bits).SAMC; }

/* Start one burst of SAMPLES_PER_BUF conversions. Reading ADxCH0DATA
 * first clears CH0RDY from the previous burst, as datasheet Example 16-6
 * does before re-triggering. */
void adc_start_burst(void)
{
    (void)ADCREG(CH0DATA);
    ADCREG(SWTRGbits).CH0TRG = 1u;
}

void adc_regs_dump(void)
{
    console_puts("[regs] adc\r\n");
    console_kv_hex("ADxCON", ADCREG(CON));
    console_kv_hex("ADxSTAT", ADCREG(STAT));
    console_kv_hex("ADxCH0CON1", ADCREG(CH0CON1));
    console_kv_hex("ADxCH0CNT", ADCREG(CH0CNT));
    console_kv_hex("ADxCH0RES", ADCREG(CH0RES));
    console_kv_hex("ADxCH0DATA", ADCREG(CH0DATA));
    console_kv_hex("IEC6", IEC6);           /* AD3CH0 enable,  bit 9     */
    console_kv_hex("IFS6", IFS6);           /* AD3CH0 flag,    bit 9     */
}

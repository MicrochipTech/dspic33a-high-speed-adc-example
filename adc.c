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
 * ADC setup - one channel, Integration mode, back-to-back inside a burst
 *
 * DS70005591D 16.4.4 (p1321) and 16.4.5 (p1322):
 *   - MODE = 10 (Integration): CNT conversions per burst, "the first
 *     conversion is initiated by a trigger selected by TRG1SRC and all
 *     subsequent conversions are executed by a trigger selected by
 *     TRG2SRC".
 *   - TRG1SRC = 000001: software trigger, ADnSWTRG (Table 16-3, p1226).
 *   - TRG2SRC = 000010: back-to-back, "re-triggered immediately after
 *     the previous conversion is finished" (Table 16-4, p1227).
 *   - TRG2SRC "are not used for a Single Conversion mode" (p1322), and
 *     000010 is reserved for TRG1SRC - so MODE = 00 cannot free-run.
 *   - IRQSEL = 0: "the channel interrupt is generated after each single
 *     conversion when result is ready in ADxRESn" (p1266). That per-
 *     conversion event is what triggers the DMA. IRQSEL = 1 would fire
 *     only once per burst.
 *   - EIEN = 0: note 4 on p1265, no early interrupt with DMA transfers.
 *   - The per-conversion result is ADxCH0RES[11:0]; ADxCH0DATA is the
 *     accumulator of the burst (p1270) and is not what we want.
 *
 * The same pattern (MODE = 2, CNT = n, TRG1SRC = 1, TRG2SRC = 2, then a
 * software trigger) is what Microchip's 40 MSPS example uses on this
 * board, and what datasheet Example 16-6 (p1331) does.
 * ------------------------------------------------------------------ */
void adc_init(uint8_t pinsel, uint8_t samc)
{
    ADCREG(CONbits).ON = 0;

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
    ADCREG(CH0CON1bits).TRG2SRC = 0x02u;       /* back-to-back          */

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
    console_puts("[adc] core ready, Integration mode, CNT 2048\r\n");
    console_kv("[adc] pinsel", pinsel);
    console_kv("[adc] samc", samc);
}

/* Input pin and sample time of channel 0. Only safe while no burst is
 * running: capture.c applies a change between two bursts. */
void adc_set_input(uint8_t pinsel, uint8_t samc)
{
    ADCREG(CH0CON1bits).PINSEL = pinsel;
    ADCREG(CH0CON1bits).SAMC   = samc;
}

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

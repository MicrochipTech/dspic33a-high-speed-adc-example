/*
 * adc_dma_40msps.c
 *
 * dsPIC33AK512MPS512 - ADC at 40 MSPS into RAM via DMA, bare metal.
 *
 * Purpose
 *   Minimal, readable starting point to measure what the device really
 *   sustains: one ADC core at full rate, DMA ping-pong into two buffers,
 *   and counters for every error the hardware can report.
 *
 *   This is a measurement harness, not a product. Nothing here has run on
 *   silicon - see README.md.
 *
 * Clocking
 *   POSC/FRC -> PLL1 -> CLKGEN1 = 200 MHz system clock
 *                    -> CLKGEN6 = 320 MHz ADC input clock
 *   TAD = 4 / 320 MHz = 12.5 ns, throughput 40 MSPS (DS70005591D, AD50/AD51).
 *   CLKGEN6 is the ADC clock source per DS70005591D Table 16-1.
 *
 * Every register write below cites the datasheet table or page it comes from.
 */

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ *
 * Configuration bits
 *
 * FICD_NOBTSWP is written as a NUMBER on purpose, because the accepted
 * symbolic names DEPEND ON THE PACK VERSION:
 *
 *   dsPIC33AK-MP_DFP 1.3.185 : ON            / OFF
 *   dsPIC33AK-MP_DFP 1.4.260 : BTSWP_ENABLED / BTSWP_DISABLED
 *
 * Same bit, same meaning, different spelling - so a file written against
 * one pack fails to compile against the other, with a message that makes
 * it look as if the value itself were invalid:
 *
 *   error: unknown value for configuration setting 'FICD_NOBTSWP'
 *
 * That is also the most likely reason an MCC-generated config_bits.c
 * suddenly stops compiling: MCC generated it against a different pack
 * than the one the build uses.
 *
 * The numeric form is accepted by every pack version. Verified: 0x0 built
 * against packs 1.3.185 and 1.4.260, and BTSWP_ENABLED built against
 * 1.4.260, all produce a bit-identical HEX file.
 *
 * FICD, mask 0x8000, value 0x0 = BOOTSWP instruction enabled.
 * (From the ATDF value-group FICD_NOBTSWP of both packs.)
 * ------------------------------------------------------------------ */
#pragma config FICD_NOBTSWP = 0x0   /* BOOTSWP enabled - see above */

/* Watchdog left under software control, so it stays off unless the
 * application turns it on via WDTCON.ON. Valid values are SW and HW
 * (ATDF value-group FWDT_WDTEN) - there is no "OFF". */
#pragma config FWDT_WDTEN = SW

/* ------------------------------------------------------------------ *
 * Tunables
 * ------------------------------------------------------------------ */

/* Samples per DMA half-buffer. 1024 x 2 byte = 2 KiB per buffer, 4 KiB
 * total, which at 40 MSPS is 25.6 us of signal per buffer. Small enough
 * to leave RAM for everything else, large enough that the completion
 * interrupt does not fire absurdly often (39 kHz per buffer). */
#define SAMPLES_PER_BUF   1024u

/* Analog input to sample: ADC1 positive input 0 (AD1AN0).
 * Input availability per package is in DS70005591D Table 16-2, p1224 ff. */
#define ADC1_PINSEL       0u

/* Sample time in TAD units, SAMC[4:0] in AD1CH0CON1 (DS70005591D p1266).
 * 0 = 0.5 TAD, the minimum, for maximum throughput. A real signal source
 * with non-negligible impedance will need more - that is the first knob
 * to turn if the results look wrong. */
#define ADC1_SAMC         0u

/* ------------------------------------------------------------------ *
 * Sample buffers
 *
 * Two buffers, alternated by the DMA completion interrupt. 16-bit words
 * because the DMA is configured for 16-bit transfers (SIZE = 1) and a
 * 12-bit result fits. Aligned to 4 bytes: the DMA writes through a
 * 32-bit path and unaligned buffers are asking for trouble.
 * ------------------------------------------------------------------ */
static volatile uint16_t buf_a[SAMPLES_PER_BUF] __attribute__((aligned(4)));
static volatile uint16_t buf_b[SAMPLES_PER_BUF] __attribute__((aligned(4)));

/* ------------------------------------------------------------------ *
 * Measurement counters - the actual point of this program
 *
 * Read these with the debugger after a run. dma_overrun and adc_overrun
 * are the numbers that answer "does the bus keep up": the datasheet
 * documents a single shared DMA data bus (DS70005591D 13.4.4, p825) but
 * gives no throughput figure.
 * ------------------------------------------------------------------ */
volatile uint32_t blocks_done   = 0;   /* completed DMA half-buffers      */
volatile uint32_t dma_overrun   = 0;   /* DMA0STAT.OVERRUN seen           */
volatile uint32_t dma_addr_err  = 0;   /* DMA0STAT.ADRERR                 */
volatile uint32_t dma_bus_err   = 0;   /* DMA0STAT.BRERR | BWERR          */
volatile uint32_t late_service  = 0;   /* ISR found next block already due */
volatile uint16_t last_sample   = 0;   /* sanity check: is data moving?    */

static volatile uint16_t *active_buf = buf_a;

/* ------------------------------------------------------------------ *
 * Clock setup
 *
 * PLL1: 8 MHz FRC -> FVCO -> PLL1 output. The divider fields are
 * PLLPRE (input divider N), PLLFBDIV (multiplier M) and POSTDIV1/2
 * (output dividers), all in PLL1DIV. The datasheet's own current-
 * consumption tables use M = 40, N = 1, N2 = 1 for FVCO = 640 MHz and a
 * 320 MHz output (DS70005591D DC111, p2008) - the same operating point
 * needed here, which is a useful cross-check that it is a sane setting.
 *
 * The PFD input must stay in 5.0 - 800 MHz (DS70005591D p777).
 * ------------------------------------------------------------------ */
static void clock_init(void)
{
    /* Make sure the FRC is running and PLL1 is enabled. */
    OSCCTRLbits.FRCEN = 1;
    while (!OSCCTRLbits.FRCRDY) { }

    /* PLL1: 8 MHz / 1 = 8 MHz PFD input, x 80 = 640 MHz FVCO,
     * POSTDIV1 = 2 -> 320 MHz PLL1 output. */
    PLL1DIVbits.PLLPRE   = 1u;    /* N  = 1                      */
    PLL1DIVbits.PLLFBDIV = 80u;   /* M  = 80  -> FVCO = 640 MHz   */
    PLL1DIVbits.POSTDIV1 = 2u;    /* /2       -> 320 MHz          */
    PLL1DIVbits.POSTDIV2 = 1u;

    PLL1CONbits.NOSC = 0x1u;      /* PLL input = FRC oscillator
                                   * (value 0x1 per ATDF CLK1_CON__COSC) */
    OSCCTRLbits.PLL1EN = 1;
    PLL1CONbits.ON     = 1;
    PLL1CONbits.OSWEN  = 1;       /* request the switch            */
    while (PLL1CONbits.OSWEN) { }
    while (!OSCCTRLbits.PLL1RDY) { }

    /* CLKGEN1 = system clock (DS70005591D 12.4.9, p795).
     * PLL1 output 320 MHz / 1.6 = 200 MHz.
     * CLKxDIV holds INTDIV plus a 9-bit FRACDIV, so 1.6 is expressible:
     * INTDIV = 1, FRACDIV = 0.6 x 512 = 307. */
    CLK1DIVbits.INTDIV  = 1u;
    CLK1DIVbits.FRACDIV = 307u;
    CLK1CONbits.NOSC    = 0x5u;   /* PLL1 Out output (ATDF: 0x5)   */
    CLK1CONbits.ON      = 1;
    CLK1CONbits.DIVSWEN = 1;
    CLK1CONbits.OSWEN   = 1;
    while (CLK1CONbits.OSWEN) { }
    while (!CLK1CONbits.CLKRDY) { }

    /* CLKGEN6 = ADC clock source (DS70005591D Table 16-1, p1223).
     * PLL1 output 320 MHz straight through: TAD = 4/320 MHz = 12.5 ns.
     * 320 MHz is the specified maximum (Table 40-24, p2016). */
    CLK6DIVbits.INTDIV  = 1u;
    CLK6DIVbits.FRACDIV = 0u;
    CLK6CONbits.NOSC    = 0x5u;   /* PLL1 Out output               */
    CLK6CONbits.ON      = 1;
    CLK6CONbits.DIVSWEN = 1;
    CLK6CONbits.OSWEN   = 1;
    while (CLK6CONbits.OSWEN) { }
    while (!CLK6CONbits.CLKRDY) { }
}

/* ------------------------------------------------------------------ *
 * ADC1 setup - one channel, continuous at maximum rate
 *
 * Trigger choice: TRG2SRC = 0b000010, "Immediate re-trigger request"
 * (DS70005591D Table 16-4, p1227). This retriggers without a period
 * calculation and is the right thing for maximum continuous rate.
 *
 * The alternative for an exact lower rate is the conversion repeat timer
 * (TRG2SRC = 0b000011) with RPTCNT in AD1CON counting ADC clock cycles,
 * 1 to 64 between triggers (DS70005591D p1258). At an 80 MHz ADC clock
 * that yields 80/n MSPS: 40, 26.67, 20, 16 ... - note that exactly
 * 25 MSPS is NOT on that grid and needs a different ADC input clock
 * (200 MHz in -> TAD 20 ns -> 50 MHz ADC clock -> /2 = 25 MSPS).
 * ------------------------------------------------------------------ */
static void adc1_init(void)
{
    AD1CONbits.ON = 0;

    /* Channel 0 configuration, AD1CH0CON1 (DS70005591D p1265 f.) */
    AD1CH0CON1bits.PINSEL  = ADC1_PINSEL; /* positive input select      */
    AD1CH0CON1bits.NINSEL  = 0u;          /* negative input = AVSS      */
    AD1CH0CON1bits.DIFF    = 0u;          /* single ended -> unsigned   */
    AD1CH0CON1bits.FRAC    = 0u;          /* integer, right aligned     */
    AD1CH0CON1bits.SAMC    = ADC1_SAMC;   /* sample time in TAD         */
    AD1CH0CON1bits.MODE    = 0u;          /* single conversion mode     */
    AD1CH0CON1bits.ACCNUM  = 0u;          /* no oversampling            */
    AD1CH0CON1bits.IRQSEL  = 0u;          /* IRQ per single conversion  */
    AD1CH0CON1bits.TRG1SRC = 0u;          /* trigger 1 off              */
    AD1CH0CON1bits.TRG2SRC = 0x02u;       /* immediate re-trigger       */

    AD1CONbits.ON = 1;
    while (!AD1CONbits.ADRDY) { }         /* wait for the core          */
}

/* ------------------------------------------------------------------ *
 * DMA channel 0: ADC1 channel 0 result -> RAM
 *
 * CHSEL = 0x2F is "ADC1 Done CH0" (ATDF value-group DMA_SEL__CHSEL).
 * SIZE = 1 selects 16-bit transfers; the DMA supports 8, 16 and 32 bit
 * (DS70005591D 13.4.2, p824), so a 12-bit result costs 2 bytes.
 *
 * AD1CH0DATA is 32 bit wide and the result is right aligned (FRAC = 0),
 * so a 16-bit read of its low half carries the sample. Worth verifying
 * on hardware - it is the one assumption here that the datasheet does
 * not state outright.
 *
 * TRMODE = Repeated Continuous keeps the channel armed; the completion
 * interrupt swaps the destination buffer.
 * ------------------------------------------------------------------ */
static void dma0_init(volatile uint16_t *dst)
{
    DMACONbits.ON = 0;

    DMA0CHbits.CHEN = 0;

    DMA0SEL = 0x2Fu;                  /* ADC1 Done CH0                  */
    DMA0SRC = (uint32_t)&AD1CH0DATA;  /* peripheral source              */
    DMA0DST = (uint32_t)dst;          /* RAM destination                */
    DMA0CNT = SAMPLES_PER_BUF;        /* words per block                */

    DMA0CHbits.SIZE    = 1u;          /* 16-bit transfers               */
    DMA0CHbits.SAMODE  = 0u;          /* source address unchanged       */
    DMA0CHbits.DAMODE  = 1u;          /* destination incremented        */
    DMA0CHbits.TRMODE  = 3u;          /* repeated continuous            */
    DMA0CHbits.RELOADD = 1u;          /* reload destination each block  */
    DMA0CHbits.RELOADC = 1u;          /* reload count each block        */
    DMA0CHbits.DONEEN  = 1u;          /* interrupt on block complete    */
    DMA0CHbits.HALFEN  = 0u;

    /* Round robin arbitration. With one channel it makes no difference,
     * but it is the setting that matters once several ADC streams share
     * the single DMA data bus (DS70005591D 13.4.4, p825). */
    DMACONbits.PRIORITY = 1u;

    DMACONbits.ON   = 1;
    DMA0CHbits.CHEN = 1;
}

/* ------------------------------------------------------------------ *
 * DMA completion ISR - swap buffers, record anything that went wrong
 *
 * Deliberately short. Everything this counts is a hardware flag, so a
 * long ISR would itself become the reason for the next overrun.
 * ------------------------------------------------------------------ */
void __attribute__((interrupt, no_auto_psv)) _DMA0Interrupt(void)
{
    if (DMA0STATbits.OVERRUN) {
        dma_overrun++;
        DMA0STATbits.OVERRUN = 1;      /* write 1 to clear             */
    }
    if (DMA0STATbits.ADRERR) {
        dma_addr_err++;
        DMA0STATbits.ADRERR = 1;
    }
    if (DMA0STATbits.BRERR || DMA0STATbits.BWERR) {
        dma_bus_err++;
        DMA0STATbits.BRERR = 1;
        DMA0STATbits.BWERR = 1;
    }

    if (DMA0STATbits.DONE) {
        /* Point the next block at the other buffer. The filled one is
         * now free for processing. */
        active_buf = (active_buf == buf_a) ? buf_b : buf_a;
        DMA0DST = (uint32_t)active_buf;

        last_sample = active_buf[0];
        blocks_done++;

        DMA0STATbits.DONE = 1;

        /* Already flagged again: the ISR did not keep up. */
        if (DMA0STATbits.DONE) {
            late_service++;
        }
    }

    IFS2bits.DMA0IF = 0;
}

/* ------------------------------------------------------------------ *
 * Process one filled buffer
 *
 * Placeholder for the customer's "+ and -" arithmetic. Written as a
 * plain accumulate so the cost of touching every sample is visible in
 * the measurement: at 40 MSPS this loop sees 40 million values per
 * second and per channel, and whether the CPU keeps up is as much a
 * question as the DMA bandwidth.
 * ------------------------------------------------------------------ */
volatile int32_t proc_result = 0;

static void process_buffer(const volatile uint16_t *b, uint32_t n)
{
    int32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += (int32_t)b[i];
    }
    proc_result = acc;
}

/* ------------------------------------------------------------------ */
int main(void)
{
    clock_init();
    adc1_init();
    dma0_init(buf_a);

    /* Enable the DMA0 interrupt. */
    IFS2bits.DMA0IF = 0;
    IEC2bits.DMA0IE = 1;

    uint32_t seen = 0;

    for (;;) {
        /* Wait for a block, then work on the buffer the DMA is not
         * currently filling. */
        if (blocks_done != seen) {
            seen = blocks_done;
            process_buffer((active_buf == buf_a) ? buf_b : buf_a,
                           SAMPLES_PER_BUF);
        }

        /* What to look at with the debugger:
         *   blocks_done  x SAMPLES_PER_BUF / elapsed time = actual rate
         *   dma_overrun  must stay 0, otherwise samples were lost
         *   late_service must stay 0, otherwise the ISR is too slow
         *   last_sample  changing means data is really moving
         */
    }

    return 0;
}

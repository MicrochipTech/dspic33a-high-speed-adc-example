/*
 * adc_dma_40msps.c
 *
 * dsPIC33AK512MPS506 Curiosity Nano (EV17P63A) - ADC at 40 MSPS into RAM
 * via DMA, bare metal. Builds unchanged for the dsPIC33AK512MPS512.
 *
 * Purpose
 *   Minimal, readable starting point to measure what the device really
 *   sustains: one ADC core at full rate, DMA into a double buffer, and
 *   counters for every error the hardware can report.
 *
 *   This is a measurement harness, not a product. Nothing here has run on
 *   silicon - see README.md.
 *
 * What you see on the board
 *   LED0 (RD0) is the only output. Slow blink (1 Hz) = everything runs and
 *   no error counter has moved. Fast blink (5 Hz) = running, but an error
 *   counter is non-zero. A counted blink pattern with a pause = the code
 *   stopped at a checkpoint; the count is the error code (table at
 *   fail() below, also in docs/TROUBLESHOOTING.md).
 *
 * Self-test
 *   Before the external input is used, the same chain samples the ADC's
 *   internal 15/16 * VDD reference (AD1AN6, DS70005591D Table 16-2) and
 *   checks that the mean of a buffer half is where it must be (~3840).
 *   That proves clock, ADC, DMA and ISR together without a signal source.
 *
 * Clocking
 *   FRC 8 MHz -> PLL1 -> CLKGEN6 = 320 MHz ADC input clock
 *             -> PLL2 -> CLKGEN1 = 200 MHz system clock
 *   TAD = 4 / 320 MHz = 12.5 ns, throughput 40 MSPS (DS70005591D, AD50/AD51).
 *   CLKGEN6 is the ADC clock source per DS70005591D Table 16-1.
 *
 * Data path
 *   The ADC channel runs in Integration mode: a software trigger starts a
 *   burst, the back-to-back trigger keeps it going for CNT conversions,
 *   and each conversion raises the "ADC1 Done CH0" event that triggers
 *   the DMA. The DMA copies the 12-bit result from AD1CH0RES into one
 *   buffer of two halves; its HALF and DONE flags tell the CPU which half
 *   is complete. At DONE the ISR starts the next burst.
 *
 *   Why not "single conversion + immediate re-trigger": the datasheet
 *   states that TRG2SRC is not used in Single Conversion mode (p1322) and
 *   lists the back-to-back value as reserved for TRG1SRC (Table 16-3,
 *   p1226). Free-running conversion exists only in the multisample modes,
 *   and there it is bounded by CNT (max 65535), so the burst has to be
 *   restarted. Tying CNT to the DMA buffer keeps ADC and DMA in step.
 *
 * Every register write below cites the datasheet table or page it comes from.
 * Revision 2026-09-22: reviewed against DS70005591D and errata DS80001162E,
 * then tailored to the EV17P63A; see README "Revision history".
 */

#include <xc.h>
#include <libpic30.h>       /* __delay32()                                 */
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

/* Samples per buffer half. The DMA fills one buffer of 2 x 1024 samples
 * (4 KiB) and raises HALF after the first half and DONE after the second,
 * so the CPU always has one complete half while the other one fills.
 * At 40 MSPS a half is 25.6 us of signal and the interrupt rate is
 * 39 kHz. One ADC burst (CNT) fills the whole buffer, so the burst is
 * restarted every 51.2 us. */
#define SAMPLES_PER_HALF  1024u
#define SAMPLES_PER_BUF   (2u * SAMPLES_PER_HALF)

/* Analog input to sample: ADC1 positive input 0 (AD1AN0). On the
 * dsPIC33AK512MPS506 that is pin RA2, on the Curiosity Nano the edge
 * connector position labelled "RA2 / AD1AN0" (user guide DS70005634A,
 * Figure 1-3). Input availability per package: DS70005591D Table 16-2. */
#define ADC1_PINSEL       0u

/* Sample time in TAD units, SAMC[4:0] in AD1CH0CON1 (DS70005591D p1266):
 * sample time = (2 * SAMC + 0.5) TAD, conversion period = (2 * SAMC + 2)
 * TAD, so the rate is 40 / (SAMC + 1) MSPS at a 320 MHz input clock.
 * 0 = 0.5 TAD, the minimum, for maximum throughput. A real signal source
 * with non-negligible impedance will need more - that is the first knob
 * to turn if the results look wrong. */
#define ADC1_SAMC         0u

/* Self-test input and window. AD1AN6 is the internal 15/16 * VDD
 * reference (Table 16-2, p1224), which the datasheet itself samples for
 * gain calibration (Example 16-3, p1328) - with SAMC = 3, because an
 * internal reference is not a 50 ohm source. Expected mean: 15/16 * 4096
 * = 3840; the window below allows +-5 % for gain and offset error. */
#define SELFTEST_PINSEL   6u
#define SELFTEST_SAMC     3u      /* 6.5 TAD = 81 ns, as in Example 16-3 */
#define SELFTEST_HALVES   4u      /* halves to skip before judging      */
#define SELFTEST_MIN      3648u   /* 3840 - 5 %                          */
#define SELFTEST_MAX      4032u   /* 3840 + 5 %                          */

/* LED0 on the Curiosity Nano is RD0 and lights when the pin is driven
 * low (DS70005634A 4.2.1: "driving the connected I/O line to GND ...
 * activates the LED"). Port D has no analog function, so no ANSEL. */
#define LED_TRIS          TRISDbits.TRISD0
#define LED_LAT           LATDbits.LATD0
#define LED_ON()          (LED_LAT = 0u)
#define LED_OFF()         (LED_LAT = 1u)
#define LED_TOGGLE()      (LED_LAT = (uint8_t)!LED_LAT)

/* Heartbeat: LED toggles every N completed halves. 39 062 halves per
 * second, so 19 531 gives a 1 Hz blink, 3 906 a 5 Hz blink. */
#define HEARTBEAT_OK      19531u
#define HEARTBEAT_ERR     3906u

/* Bound for every hardware wait loop, in loop iterations. A step that
 * needs longer than this has failed; fail() then reports which one. */
#define WAIT_LIMIT        2000000u

/* ------------------------------------------------------------------ *
 * Sample buffer
 *
 * One buffer, two halves. 16-bit words because the DMA is configured for
 * 16-bit transfers (SIZE = 1) and the 12-bit result in AD1CH0RES[11:0]
 * fits. Aligned to 4 bytes: the DMA writes through a 32-bit path and
 * unaligned buffers are asking for trouble.
 * ------------------------------------------------------------------ */
static volatile uint16_t buf[SAMPLES_PER_BUF] __attribute__((aligned(4)));

/* RAM window for the DMA address limit registers. __DATA_BASE and
 * __DATA_LENGTH come from the device header (0x4000 and 0x10000 for the
 * 64 KB parts, matching p33AK512MPS506.gld), so the window follows the
 * device instead of being a magic number. */
#if !defined(__DATA_BASE) || !defined(__DATA_LENGTH)
#error "__DATA_BASE / __DATA_LENGTH not provided by the device header"
#endif

/* ------------------------------------------------------------------ *
 * Measurement counters - the actual point of this program
 *
 * Read these with the debugger after a run. dma_overrun is the number
 * that answers "does the bus keep up": the datasheet documents a single
 * shared DMA data bus (DS70005591D 13.4.4, p825) but gives no throughput
 * figure.
 * ------------------------------------------------------------------ */
volatile uint32_t blocks_done   = 0;   /* completed buffer halves          */
volatile uint32_t dma_overrun   = 0;   /* DMA0STAT.OVERRUN seen            */
volatile uint32_t dma_addr_err  = 0;   /* DMA0STAT.ADRERR != 0             */
volatile uint32_t dma_bus_err   = 0;   /* DMA0STAT.BRERR | BWERR (note 1)  */
volatile uint32_t late_service  = 0;   /* HALF and DONE pending together   */
volatile uint32_t proc_missed   = 0;   /* main() skipped a completed half  */
volatile uint16_t last_sample   = 0;   /* sanity check: is data moving?    */
volatile uint32_t ready_half    = 0;   /* 0 = buf[0..], 1 = buf[1024..]    */
volatile uint32_t selftest_mean = 0;   /* mean seen on AD1AN6, ~3840       */
volatile uint32_t fail_code     = 0;   /* != 0: stopped, see fail()        */

/* Note 1: errata DS80001162E item 2 - BRERR is only set when RETEN = 1,
 * and RETEN also raises a trap. This example leaves RETEN = 0, so
 * dma_bus_err effectively counts write errors (BWERR) only. */

/* Channel reconfiguration requested by main(), applied by the ISR
 * between two bursts, when the channel is idle. */
static volatile bool    pinsel_switch_pending = false;
static volatile uint8_t pinsel_next = ADC1_PINSEL;
static volatile uint8_t samc_next   = ADC1_SAMC;

/* NOSC values, from the ATDF value-group CLK1_CON__COSC. */
#define NOSC_FRC        0x1u
#define NOSC_PLL1_OUT   0x5u
#define NOSC_PLL2_OUT   0x6u

/* ------------------------------------------------------------------ *
 * Stop here and say why - with the LED, because at this point there
 * may be no debugger attached and no clock to speak of.
 *
 *   code  meaning                                   where
 *   1     PLL1 (ADC clock) did not configure/lock   clock_init()
 *   2     PLL2 (system clock) did not configure/lock clock_init()
 *   3     CLKGEN1 did not switch to PLL2             clock_init()
 *   4     CLKGEN6 did not switch to PLL1             clock_init()
 *   5     ADC core never became ready (ADRDY)        adc1_init()
 *   6     no DMA blocks arrived (nothing moves)      self-test / run
 *   7     self-test value out of range               self-test
 *   8     DMA channel switched itself off (CHEN = 0) self-test / run
 *
 * Pattern: <code> short blinks, one long pause, repeat. The blink speed
 * depends on which clock the CPU is on at the time; the count is what
 * counts.
 * ------------------------------------------------------------------ */
static void fail(uint32_t code)
{
    fail_code = code;
    IEC2bits.DMA0IE = 0;
    DMA0CHbits.CHEN = 0;

    /* 100 ms in CPU cycles: 200 MHz once PLL2 drives CLKGEN1, else the
     * 8 MHz FRC we started on. */
    const uint32_t ms100 = (CLK1CONbits.COSC == NOSC_PLL2_OUT)
                           ? 20000000ul : 800000ul;
    LED_TRIS = 0u;
    for (;;) {
        for (uint32_t i = 0; i < code; i++) {
            LED_ON();  __delay32(2u * ms100);
            LED_OFF(); __delay32(2u * ms100);
        }
        __delay32(10u * ms100);
    }
}

/* Wait until a condition becomes false, or give up with a code. */
#define WAIT_WHILE(cond, code)                              \
    do {                                                    \
        uint32_t n_ = WAIT_LIMIT;                           \
        while (cond) {                                      \
            if (--n_ == 0u) { fail(code); }                 \
        }                                                   \
    } while (0)

/* ------------------------------------------------------------------ *
 * Clock setup
 *
 * Two PLLs, because the two rates this example needs are both exact
 * multiples of the 8 MHz FRC and neither needs a fractional divider:
 *
 *   PLL1 -> 320 MHz -> CLKGEN6 -> ADC   (TAD = 4/320 MHz = 12.5 ns)
 *   PLL2 -> 200 MHz -> CLKGEN1 -> CPU, DMA and the standard peripherals
 *
 * THE SWITCHING ORDER IS NOT OPTIONAL. DS70005591D page 778 spells it
 * out, and skipping a step does not fail loudly - it leaves the old
 * divider values in place and the part runs at the wrong speed:
 *
 *   a) set PLLSWEN  -> allows the input and feedback dividers to update
 *   c) set FOUTSWEN -> allows the output dividers to update
 *   d) select the source in NOSC
 *   e) set OSWEN    -> perform the switch
 *
 * Each of those bits clears itself when its step has completed, so each
 * one is followed by a (bounded) wait.
 *
 * Also from page 778: "The output dividers POSTDIV1 and POSTDIV2 should
 * not be changed while the PLL is operating", and POSTDIV1 must be >=
 * POSTDIV2.
 *
 * The divider values below are the ones Microchip's own MCC-generated
 * example uses for this part, which is the reason to prefer them over an
 * equally valid arithmetic alternative - they have run on hardware:
 *   https://github.com/microchip-pic-avr-examples/dspic33ak-curiosity-adc-40msps
 *
 *   PLL1DIV = 0x0100C829 : N1=1, M=200, POSTDIV1=5, POSTDIV2=1
 *                          8 MHz -> FVCO 1600 MHz -> 320 MHz
 *   PLL2DIV = 0x01007D29 : N1=1, M=125, POSTDIV1=5, POSTDIV2=1
 *                          8 MHz -> FVCO 1000 MHz -> 200 MHz
 *
 * Bit layout of PLLxDIV (ATDF): POSTDIV2[2:0], POSTDIV1[5:3],
 * PLLFBDIV[16:8], PLLPRE[27:24].
 *
 * Constraints checked against Table 40-23 and page 777: F_PFD >= 5 MHz,
 * F_VCO 500...1600 MHz, M in 16...320, POSTDIV1 >= POSTDIV2.
 * ------------------------------------------------------------------ */
static void clock_init(void)
{
    /* If the system clock is currently running off a PLL, park it on the
     * FRC first. Changing PLL settings underneath a running CPU clock can
     * overclock the core - this matters on a debugger restart, where the
     * part is not freshly reset. (The MCC example does the same.) */
    if ((CLK1CONbits.COSC >= NOSC_PLL1_OUT) && (CLK1CONbits.COSC <= 0x8u)) {
        CLK1CONbits.NOSC  = NOSC_FRC;
        CLK1CONbits.OSWEN = 1u;
        WAIT_WHILE(CLK1CONbits.OSWEN, 3u);
    }

    /* ---- PLL1: 320 MHz for the ADC ---- */
    PLL1CON = 0x8100u;          /* ON = 1, NOSC = FRC                   */
    PLL1DIV = 0x0100C829u;      /* N1=1, M=200, POSTDIV1=5, POSTDIV2=1  */

    PLL1CONbits.PLLSWEN  = 1u;  /* (a) apply input and feedback dividers */
    WAIT_WHILE(PLL1CONbits.PLLSWEN, 1u);
    PLL1CONbits.FOUTSWEN = 1u;  /* (c) apply output dividers             */
    WAIT_WHILE(PLL1CONbits.FOUTSWEN, 1u);
    PLL1CONbits.OSWEN    = 1u;  /* (e) switch                            */
    WAIT_WHILE(PLL1CONbits.OSWEN, 1u);
    WAIT_WHILE(!OSCCTRLbits.PLL1RDY, 1u);

    VCO1DIV = 0x10000u;         /* VCO divider output, unused here       */
    PLL1CONbits.DIVSWEN = 1u;
    WAIT_WHILE(PLL1CONbits.DIVSWEN, 1u);

    /* ---- PLL2: 200 MHz for the system clock ---- */
    PLL2CON = 0x8100u;
    PLL2DIV = 0x01007D29u;      /* N1=1, M=125, POSTDIV1=5, POSTDIV2=1  */

    PLL2CONbits.PLLSWEN  = 1u;
    WAIT_WHILE(PLL2CONbits.PLLSWEN, 2u);
    PLL2CONbits.FOUTSWEN = 1u;
    WAIT_WHILE(PLL2CONbits.FOUTSWEN, 2u);
    PLL2CONbits.OSWEN    = 1u;
    WAIT_WHILE(PLL2CONbits.OSWEN, 2u);
    WAIT_WHILE(!OSCCTRLbits.PLL2RDY, 2u);

    VCO2DIV = 0x10000u;
    PLL2CONbits.DIVSWEN = 1u;
    WAIT_WHILE(PLL2CONbits.DIVSWEN, 2u);

    /* ---- CLKGEN1 = system clock, from PLL2, no divider ----
     * DS70005591D 12.4.9, p795: "Clock Generator 1 is the clock source
     * for the system clock (sys_clk) and peripheral clock." */
    CLK1CON = 0x129600u;        /* NOSC = PLL2 out, ON, backup BFRC, FSCM */
    CLK1DIV = 0u;               /* 200 MHz straight through               */
    CLK1CONbits.OSWEN = 1u;
    WAIT_WHILE(CLK1CONbits.OSWEN, 3u);

    /* ---- CLKGEN6 = ADC clock, from PLL1, no divider ----
     * DS70005591D Table 16-1, p1223 names CLKGEN6 as the ADC clock
     * source, range 32...320 MHz. 320 MHz is the maximum (Table 40-24,
     * p2016) and gives TAD = 12.5 ns, hence 40 MSPS (AD50/AD51). */
    CLK6CON = 0x29500u;         /* NOSC = PLL1 out, ON                   */
    CLK6DIV = 0u;               /* 320 MHz straight through              */
    CLK6CONbits.OSWEN = 1u;
    WAIT_WHILE(CLK6CONbits.OSWEN, 4u);
}

/* ------------------------------------------------------------------ *
 * ADC1 setup - one channel, Integration mode, back-to-back inside a burst
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
 *   - The per-conversion result is AD1CH0RES[11:0]; AD1CH0DATA is the
 *     accumulator of the burst (p1270) and is not what we want.
 *
 * The same pattern (MODE = 2, CNT = n, TRG1SRC = 1, TRG2SRC = 2, then a
 * software trigger) is what Microchip's 40 MSPS example uses on
 * hardware, and what datasheet Example 16-6 (p1331) does.
 * ------------------------------------------------------------------ */
static void adc1_init(uint8_t pinsel, uint8_t samc)
{
    AD1CONbits.ON = 0;

    /* Channel 0 configuration, AD1CH0CON1 (DS70005591D p1265 f.) */
    AD1CH0CON1bits.PINSEL  = pinsel;      /* positive input select      */
    AD1CH0CON1bits.NINSEL  = 0u;          /* negative input = AVSS      */
    AD1CH0CON1bits.DIFF    = 0u;          /* single ended -> unsigned   */
    AD1CH0CON1bits.FRAC    = 0u;          /* integer, right aligned     */
    AD1CH0CON1bits.SAMC    = samc;        /* sample time in TAD         */
    AD1CH0CON1bits.MODE    = 2u;          /* Integration: CNT per burst */
    AD1CH0CON1bits.ACCNUM  = 0u;          /* oversampling only, unused  */
    AD1CH0CON1bits.IRQSEL  = 0u;          /* event per conversion (RES) */
    AD1CH0CON1bits.EIEN    = 0u;          /* no early interrupt w/ DMA  */
    AD1CH0CON1bits.TRG1SRC = 0x01u;       /* software trigger starts    */
    AD1CH0CON1bits.TRG2SRC = 0x02u;       /* back-to-back continues     */

    /* Conversions per burst. One burst fills the whole DMA buffer, so
     * the DMA DONE interrupt is also the moment to start the next one.
     * CNT[15:0] in AD1CH0CNT (p1272), max 65535. */
    AD1CH0CNT = SAMPLES_PER_BUF;

    AD1CONbits.ON = 1;
    WAIT_WHILE(!AD1CONbits.ADRDY, 5u);    /* wait for the core          */
}

/* Start one burst of SAMPLES_PER_BUF conversions. Reading AD1CH0DATA
 * first clears CH0RDY from the previous burst, as datasheet Example 16-6
 * does before re-triggering. */
static inline void adc1_start_burst(void)
{
    (void)AD1CH0DATA;
    AD1SWTRGbits.CH0TRG = 1u;
}

/* ------------------------------------------------------------------ *
 * DMA channel 0: ADC1 channel 0 result -> RAM
 *
 * CHSEL = 0x2F is "ADC1 Done CH0" (ATDF value-group DMA_SEL__CHSEL).
 * SIZE = 1 selects 16-bit transfers; the DMA supports 8, 16 and 32 bit
 * (DS70005591D 13.4.2, p824 and p812), so a 12-bit result costs 2 bytes.
 * AD1CH0RES holds RES[11:0] in the low half and RESF[11:0] in bits
 * 31:20 (p1229), so the 16-bit read of the low half is the sample.
 *
 * DMALOW / DMAHIGH MUST be set. They reset to 0, every transaction is
 * checked against them (13.4.8.1 p829, step 5), and an access above
 * DMAHIGH sets ADRERR = 10 and clears CHEN (p810, p826). With the reset
 * values the very first sample would disable the channel. Every
 * datasheet example (p832 ff.) and MCC set them.
 *
 * TRMODE = Repeated Continuous with RELOADD/RELOADC restarts at the
 * buffer start after each block on its own (p812, p829 step 4). HALFEN
 * and DONEEN give one interrupt per half (13.6.1.2, p848). No address
 * is ever rewritten from software while the channel runs.
 *
 * DMAxSTAT flags are "R/C/HS" - clearable by writing 0 (legend p815,
 * Example 13-4 p835: "DMA0STATbits.DONE=0"). Writing 1 does not clear.
 * ------------------------------------------------------------------ */
static void dma0_init(void)
{
    DMACONbits.ON = 0;
    DMA0CHbits.CHEN = 0;

    /* Address window = the device's data RAM (p809 f.). */
    DMALOW  = (uint32_t)__DATA_BASE;
    DMAHIGH = (uint32_t)__DATA_BASE + (uint32_t)__DATA_LENGTH - 1u; /* 0x13FFF */

    DMA0SEL = 0x2Fu;                  /* ADC1 Done CH0                  */
    DMA0SRC = (uint32_t)&AD1CH0RES;   /* per-conversion result          */
    DMA0DST = (uint32_t)buf;          /* RAM destination                */
    DMA0CNT = SAMPLES_PER_BUF;        /* transactions per block         */
    DMA0STAT = 0u;                    /* clear any stale flags          */

    DMA0CH = 0u;
    DMA0CHbits.SIZE    = 1u;          /* 16-bit transfers               */
    DMA0CHbits.SAMODE  = 0u;          /* source address unchanged       */
    DMA0CHbits.DAMODE  = 1u;          /* destination incremented        */
    DMA0CHbits.TRMODE  = 3u;          /* repeated continuous            */
    DMA0CHbits.RELOADD = 1u;          /* reload destination each block  */
    DMA0CHbits.RELOADC = 1u;          /* reload count each block        */
    DMA0CHbits.HALFEN  = 1u;          /* interrupt at half              */
    DMA0CHbits.DONEEN  = 1u;          /* interrupt on block complete    */
    DMA0CHbits.RETEN   = 0u;          /* see note 1 at the counters     */

    /* Round robin arbitration. With one channel it makes no difference,
     * but it is the setting that matters once several ADC streams share
     * the single DMA data bus (DS70005591D 13.4.4, p825). */
    DMACONbits.PRIORITY = 1u;

    DMACONbits.ON   = 1;
    DMA0CHbits.CHEN = 1;
}

/* ------------------------------------------------------------------ *
 * DMA interrupt - one per buffer half
 *
 * HALF: the first half is complete, the DMA is filling the second.
 * DONE: the second half is complete, the DMA has reloaded to the start
 *       and the ADC burst has ended - apply a pending input change and
 *       start the next burst here.
 *
 * Deliberately short. Everything this counts is a hardware flag, so a
 * long ISR would itself become the reason for the next overrun.
 * ------------------------------------------------------------------ */
void __attribute__((interrupt, no_auto_psv)) _DMA0Interrupt(void)
{
    const uint32_t st = DMA0STAT;     /* one snapshot, then act on it   */

    if (st & _DMA0STAT_OVERRUN_MASK) {
        /* Triggered while the previous transfer was still in progress
         * (p816): the bus did not keep up. This is the measurement. */
        dma_overrun++;
        DMA0STATbits.OVERRUN = 0;
    }
    if (st & _DMA0STAT_ADRERR_MASK) {
        dma_addr_err++;
        DMA0STATbits.ADRERR = 0;
    }
    if (st & (_DMA0STAT_BRERR_MASK | _DMA0STAT_BWERR_MASK)) {
        dma_bus_err++;
        DMA0STATbits.BRERR = 0;
        DMA0STATbits.BWERR = 0;
    }

    /* Both halves pending at once means this ISR arrived more than one
     * half (25.6 us) late and the first half has already been
     * overwritten by the DMA reload. */
    if ((st & _DMA0STAT_HALF_MASK) && (st & _DMA0STAT_DONE_MASK)) {
        late_service++;
    }

    if (st & _DMA0STAT_HALF_MASK) {
        DMA0STATbits.HALF = 0;
        ready_half  = 0u;
        last_sample = buf[SAMPLES_PER_HALF - 1u];
        blocks_done++;
    }
    if (st & _DMA0STAT_DONE_MASK) {
        DMA0STATbits.DONE = 0;
        ready_half  = 1u;
        last_sample = buf[SAMPLES_PER_BUF - 1u];
        blocks_done++;

        /* The channel is idle between bursts: this is the only safe
         * moment to change its input or sample time. */
        if (pinsel_switch_pending) {
            AD1CH0CON1bits.PINSEL = pinsel_next;
            AD1CH0CON1bits.SAMC   = samc_next;
            pinsel_switch_pending = false;
        }
        adc1_start_burst();           /* next SAMPLES_PER_BUF samples   */
    }

    IFS2bits.DMA0IF = 0;
}

/* ------------------------------------------------------------------ *
 * Process one completed buffer half
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

static uint32_t half_mean(const volatile uint16_t *b, uint32_t n)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += b[i];
    }
    return acc / n;
}

/* Wait until blocks_done passes a value. Gives up with code 6 (nothing
 * moves) or 8 (the DMA switched itself off, e.g. on an address fault). */
static void wait_for_blocks(uint32_t target)
{
    uint32_t n = WAIT_LIMIT;
    while (blocks_done < target) {
        if (DMA0CHbits.CHEN == 0u) { fail(8u); }
        if (--n == 0u)             { fail(6u); }
    }
}

/* ------------------------------------------------------------------ */
int main(void)
{
    LED_OFF();
    LED_TRIS = 0u;

    clock_init();

    /* ---- Self-test on the internal 15/16 * VDD reference ----
     * Same clock, ADC, DMA and ISR as the real measurement, only the
     * input differs. If the mean is right, the whole chain works. */
    adc1_init(SELFTEST_PINSEL, SELFTEST_SAMC);
    dma0_init();

    IFS2bits.DMA0IF = 0;              /* IEC2 bit 13, IPC9 default 4    */
    IEC2bits.DMA0IE = 1;

    adc1_start_burst();               /* first burst; the ISR keeps going */

    wait_for_blocks(SELFTEST_HALVES);
    selftest_mean = half_mean(&buf[ready_half ? SAMPLES_PER_HALF : 0u],
                              SAMPLES_PER_HALF);
    if ((selftest_mean < SELFTEST_MIN) || (selftest_mean > SELFTEST_MAX)) {
        fail(7u);
    }

    /* ---- Switch to the external input, between two bursts ---- */
    pinsel_next = ADC1_PINSEL;
    samc_next   = ADC1_SAMC;
    pinsel_switch_pending = true;
    {
        const uint32_t now = blocks_done;
        wait_for_blocks(now + 2u * SELFTEST_HALVES);   /* let it settle */
    }
    LED_ON();

    /* ---- Measurement ---- */
    uint32_t seen      = blocks_done;
    uint32_t idle      = 0;

    for (;;) {
        /* Wait for a half, then work on it while the DMA fills the
         * other one. If more than one half completed since the last
         * pass, the older one is already gone - count that. */
        const uint32_t done = blocks_done;
        if (done != seen) {
            if ((done - seen) > 1u) {
                proc_missed += (done - seen) - 1u;
            }
            seen = done;
            idle = 0;
            process_buffer(&buf[ready_half ? SAMPLES_PER_HALF : 0u],
                           SAMPLES_PER_HALF);

            /* Heartbeat: slow while clean, fast once any error counter
             * has moved. */
            const bool clean = (dma_overrun | dma_addr_err | dma_bus_err |
                                late_service | proc_missed) == 0u;
            if ((done % (clean ? HEARTBEAT_OK : HEARTBEAT_ERR)) == 0u) {
                LED_TOGGLE();
            }
        } else {
            /* The stream stopped: burst restart lost, or the DMA shut
             * itself off. Say so instead of sitting here silently. */
            if (DMA0CHbits.CHEN == 0u) { fail(8u); }
            if (++idle > WAIT_LIMIT)   { fail(6u); }
        }

        /* What to look at with the debugger:
         *   blocks_done   x SAMPLES_PER_HALF / elapsed time = actual rate
         *                 (includes the re-trigger gap once per buffer)
         *   dma_overrun   must stay 0, otherwise the DMA bus lost samples
         *   late_service  must stay 0, otherwise the ISR is too slow
         *   proc_missed   must stay 0, otherwise main() is too slow
         *   last_sample   changing means data is really moving
         *   selftest_mean ~3840 = the chain was proven before AN0 was used
         *   fail_code     0 while running; the LED pattern otherwise
         */
    }

    return 0;
}

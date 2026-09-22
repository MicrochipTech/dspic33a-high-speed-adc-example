/*
 * adc_dma_40msps.c
 *
 * dsPIC33AK512MPS512 on the dsPIC33 Curiosity Platform Development Board
 * (EV74H48A) - ADC at 40 MSPS into RAM via DMA, bare metal.
 *
 * Purpose
 *   Minimal, readable starting point to measure what the device really
 *   sustains: one ADC core at full rate, DMA into a double buffer, and
 *   counters for every error the hardware can report. A command console
 *   on the board's PKOB4 USB-UART channel (cli.c) controls it.
 *
 * Files
 *   main.c            start-up sequence and the main loop
 *   adc_dma_40msps.c  this file: clock, ADC, DMA, ISR, self-test, LED
 *   cli.c             the console (UART1, receive interrupt, commands)
 *   cmd_parser.c/.h   the command parser, unchanged from its repository
 *
 *   This is a measurement harness, not a product. Nothing here has run on
 *   silicon - see README.md.
 *
 * What you see on the board
 *   LED0 (RC8) is the status output. Slow blink (1 Hz) = everything runs
 *   and no error counter has moved. Fast blink (5 Hz) = running, but an
 *   error counter is non-zero. A counted blink pattern with a pause = the
 *   code stopped at a checkpoint; the count is the error code (table at
 *   fail() below, also in docs/TROUBLESHOOTING.md).
 *
 * Self-test
 *   Before the external input is used, the same chain samples the ADC's
 *   internal 15/16 * VDD reference (ADxAN6, DS70005591D Table 16-2) and
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
 *   and each conversion raises the "ADCn Done CH0" event that triggers
 *   the DMA. The DMA copies the 12-bit result from ADnCH0RES into one
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
 * Revision history in README.md.
 */

#include <xc.h>
#include <libpic30.h>       /* __delay32()                                 */
#include "adc_dma_40msps.h"

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
 * ADC core selection
 *
 * The five ADC cores have identical register sets, only the prefix
 * differs (AD1..., AD5...). ADCREG(x) expands to the register of the core
 * selected by ADC_INSTANCE in adc_dma_40msps.h, e.g. ADCREG(CH0CON1bits).
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

/* ------------------------------------------------------------------ *
 * Tunables
 * ------------------------------------------------------------------ */

/* Self-test input and window. ADxAN6 is the internal 15/16 * VDD
 * reference on every core and package (Table 16-2, p1224), which the
 * datasheet itself samples for gain calibration (Example 16-3, p1328) -
 * with SAMC = 3, because an internal reference is not a 50 ohm source.
 * Expected mean: 15/16 * 4096 = 3840; the window allows +-5 %. */
#define SELFTEST_PINSEL   6u
#define SELFTEST_SAMC     3u      /* 6.5 TAD = 81 ns, as in Example 16-3 */
#define SELFTEST_HALVES   6u      /* halves to let the switch settle    */
#define SELFTEST_MIN      3648u   /* 3840 - 5 %                          */
#define SELFTEST_MAX      4032u   /* 3840 + 5 %                          */

/* LED0 on the Curiosity Platform Development Board is RC8, DIM pin 28
 * (DIM info sheet DS70005563A, Table 1). The green LEDs are driven high
 * to light (user guide DS70005562D 2.5; Microchip's own example on this
 * board reports "LED0 HIGH during sampling"). Port C has no ANSEL. */
#define LED_TRIS          TRISCbits.TRISC8
#define LED_LAT           LATCbits.LATC8
#define LED_ON()          (LED_LAT = 1u)
#define LED_OFF()         (LED_LAT = 0u)
#define LED_TOGGLE()      (LED_LAT = (uint8_t)!LED_LAT)

/* Heartbeat: LED toggles every N completed halves. 39 062 halves per
 * second, so 19 531 gives a 1 Hz blink, 3 906 a 5 Hz blink. */
#define HEARTBEAT_OK      19531u
#define HEARTBEAT_ERR     3906u


/* ------------------------------------------------------------------ *
 * Sample buffer
 *
 * One buffer, two halves. 16-bit words because the DMA is configured for
 * 16-bit transfers (SIZE = 1) and the 12-bit result in ADxCH0RES[11:0]
 * fits. Aligned to 4 bytes: the DMA writes through a 32-bit path and
 * unaligned buffers are asking for trouble.
 * ------------------------------------------------------------------ */
volatile uint16_t buf[SAMPLES_PER_BUF] __attribute__((aligned(4)));

/* RAM window for the DMA address limit registers. __DATA_BASE and
 * __DATA_LENGTH come from the device header (0x4000 and 0x10000 for the
 * 64 KB parts, matching p33AK512MPS512.gld), so the window follows the
 * device instead of being a magic number. */
#if !defined(__DATA_BASE) || !defined(__DATA_LENGTH)
#error "__DATA_BASE / __DATA_LENGTH not provided by the device header"
#endif

/* ------------------------------------------------------------------ *
 * Measurement counters - the actual point of this program
 *
 * Read these with the debugger or the "status" command. dma_overrun is
 * the number that answers "does the bus keep up": the datasheet documents
 * a single shared DMA data bus (DS70005591D 13.4.4, p825) but gives no
 * throughput figure.
 * ------------------------------------------------------------------ */
volatile uint32_t blocks_done   = 0;   /* completed buffer halves          */
volatile uint32_t dma_overrun   = 0;   /* DMA0STAT.OVERRUN seen            */
volatile uint32_t dma_addr_err  = 0;   /* DMA0STAT.ADRERR != 0             */
volatile uint32_t dma_bus_err   = 0;   /* DMA0STAT.BRERR | BWERR (note 1)  */
volatile uint32_t late_service  = 0;   /* HALF and DONE pending together   */
volatile uint32_t proc_missed   = 0;   /* main() skipped a completed half  */
volatile uint16_t last_sample   = 0;   /* sanity check: is data moving?    */
volatile uint32_t ready_half    = 0;   /* 0 = buf[0..], 1 = buf[1024..]    */
volatile uint32_t selftest_mean = 0;   /* mean seen on ADxAN6, ~3840       */
volatile uint32_t fail_code     = 0;   /* != 0: stopped, see fail()        */
volatile int32_t  proc_result   = 0;   /* output of process_buffer()       */

/* Note 1: errata DS80001162E item 2 - BRERR is only set when RETEN = 1,
 * and RETEN also raises a trap. This example leaves RETEN = 0, so
 * dma_bus_err effectively counts write errors (BWERR) only. */

/* Run control. run_enabled is what the console sets; burst_active says
 * whether a burst is in flight, so that "start" during a burst does not
 * trigger a second one on top. */
static volatile bool    run_enabled  = false;
static volatile bool    burst_active = false;

/* Channel reconfiguration requested by the console or the self-test,
 * applied by the ISR between two bursts, when the channel is idle. */
static volatile bool    switch_pending = false;
static volatile uint8_t pinsel_next    = ADC_PINSEL;
static volatile uint8_t samc_next      = ADC_SAMC;
static volatile uint8_t pinsel_cur     = ADC_PINSEL;
static volatile uint8_t samc_cur       = ADC_SAMC;

static volatile uint8_t led_auto       = 2u;   /* 0 off, 1 on, 2 auto     */
static uint32_t         seen_blocks    = 0;

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
 *   5     ADC core never became ready (ADRDY)        adc_init()
 *   6     no DMA blocks arrived (nothing moves)      self-test / run
 *   7     self-test value out of range               self-test
 *   8     DMA channel switched itself off (CHEN = 0) self-test / run
 *
 * Pattern: <code> short blinks, one long pause, repeat. The blink speed
 * depends on which clock the CPU is on at the time; the count is what
 * counts.
 * ------------------------------------------------------------------ */
static const char *const fail_text[] = {
    "no error",
    "PLL1 (ADC clock) did not configure or lock",
    "PLL2 (system clock) did not configure or lock",
    "CLKGEN1 did not switch",
    "CLKGEN6 did not switch to PLL1",
    "ADC core never reported ready (ADRDY)",
    "no DMA blocks arrived, or the stream stopped",
    "self-test mean outside 3648..4032",
    "DMA channel switched itself off (CHEN = 0)",
};

void fail(uint32_t code)
{
    fail_code = code;
    IEC2bits.DMA0IE = 0;
    DMA0CHbits.CHEN = 0;

    /* Say why, with everything a reader needs, before blinking forever.
     * The UART is up from the first line of main() on, so this works for
     * the clock steps too. */
    console_sync_baud();
    console_puts("\r\n");
    console_kv("[FAIL] code", code);
    console_puts("[FAIL] ");
    console_puts((code < 9u) ? fail_text[code] : "unknown code");
    console_puts("\r\n");
    regs_dump();
    console_puts("[FAIL] LED0 blinks the code from now on\r\n");

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
 * example uses for this part on this board, which is the reason to prefer
 * them over an equally valid arithmetic alternative - they have run on
 * hardware:
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
void clock_init(void)
{
    /* If the system clock is currently running off a PLL, park it on the
     * FRC first. Changing PLL settings underneath a running CPU clock can
     * overclock the core - this matters on a debugger restart, where the
     * part is not freshly reset. (The MCC example does the same.) */
    console_kv_hex("[clk] CLK1CON at entry", CLK1CON);
    if ((CLK1CONbits.COSC >= NOSC_PLL1_OUT) && (CLK1CONbits.COSC <= 0x8u)) {
        console_puts("[clk] system clock on a PLL, parking on FRC\r\n");
        CLK1CONbits.NOSC  = NOSC_FRC;
        CLK1CONbits.OSWEN = 1u;
        WAIT_WHILE(CLK1CONbits.OSWEN, 3u);
    }

    /* ---- PLL1: 320 MHz for the ADC ---- */
    PLL1CON = 0x8100u;          /* ON = 1, NOSC = FRC                   */

    /* Two Microchip references disagree here. The MCC example enables
     * the PLL with PLLxCON.ON alone and applies the dividers first. The
     * datasheet's own clock example (Example 16-3, p1328) additionally
     * sets OSCCTRL.PLLxEN and waits for PLLxRDY before it touches any
     * divider. Doing both cannot hurt: PLLxEN is set here, and the wait
     * is bounded and non-fatal - the trace says which way it went. */
    OSCCTRLbits.PLL1EN = 1u;
    {
        uint32_t n = 200000u;
        while (!OSCCTRLbits.PLL1RDY && (--n != 0u)) { }
    }
    console_puts(OSCCTRLbits.PLL1RDY ? "[clk] PLL1 ready with POR dividers\r\n"
                                     : "[clk] PLL1 not ready yet, continuing\r\n");

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
    console_puts("[clk] PLL1 locked, 320 MHz\r\n");

    /* ---- PLL2: 200 MHz for the system clock ---- */
    PLL2CON = 0x8100u;
    OSCCTRLbits.PLL2EN = 1u;    /* see PLL1 above                        */
    {
        uint32_t n = 200000u;
        while (!OSCCTRLbits.PLL2RDY && (--n != 0u)) { }
    }
    console_puts(OSCCTRLbits.PLL2RDY ? "[clk] PLL2 ready with POR dividers\r\n"
                                     : "[clk] PLL2 not ready yet, continuing\r\n");

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
    console_puts("[clk] PLL2 locked, 200 MHz\r\n");

    /* ---- CLKGEN1 = system clock, from PLL2, no divider ----
     * DS70005591D 12.4.9, p795: "Clock Generator 1 is the clock source
     * for the system clock (sys_clk) and peripheral clock." */
    CLK1CON = 0x129600u;        /* NOSC = PLL2 out, ON, backup BFRC, FSCM */
    CLK1DIV = 0u;               /* 200 MHz straight through               */
    CLK1CONbits.OSWEN = 1u;
    WAIT_WHILE(CLK1CONbits.OSWEN, 3u);
    /* From here on the CPU runs at 200 MHz and the UART's baud generator
     * is off by 25x until cli_init() re-sets it - so no trace output
     * until then. */

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

    pinsel_cur = pinsel;
    samc_cur   = samc;

    ADCREG(CONbits).ON = 1;
    WAIT_WHILE(!ADCREG(CONbits).ADRDY, 5u);    /* wait for the core     */
    console_puts("[adc] core ready, Integration mode, CNT 2048\r\n");
    console_kv("[adc] pinsel", pinsel);
    console_kv("[adc] samc", samc);
}

/* Start one burst of SAMPLES_PER_BUF conversions. Reading ADxCH0DATA
 * first clears CH0RDY from the previous burst, as datasheet Example 16-6
 * does before re-triggering. */
static inline void adc_start_burst(void)
{
    (void)ADCREG(CH0DATA);
    burst_active = true;
    ADCREG(SWTRGbits).CH0TRG = 1u;
}

/* ------------------------------------------------------------------ *
 * DMA channel 0: ADCn channel 0 result -> RAM
 *
 * CHSEL = "ADCn Done CH0" (ATDF value-group DMA_SEL__CHSEL, see the
 * table at ADCREG above). SIZE = 1 selects 16-bit transfers; the DMA
 * supports 8, 16 and 32 bit (DS70005591D 13.4.2, p824 and p812), so a
 * 12-bit result costs 2 bytes. ADxCH0RES holds RES[11:0] in the low half
 * and RESF[11:0] in bits 31:20 (p1229), so the 16-bit read of the low
 * half is the sample.
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
void dma0_init(void)
{
    DMACONbits.ON = 0;
    DMA0CHbits.CHEN = 0;

    /* Address window = the device's data RAM (p809 f.). */
    DMALOW  = (uint32_t)__DATA_BASE;
    DMAHIGH = (uint32_t)__DATA_BASE + (uint32_t)__DATA_LENGTH - 1u; /* 0x13FFF */

    DMA0SEL = DMA_TRIG_ADC_CH0;             /* ADCn Done CH0            */
    DMA0SRC = (uint32_t)&ADCREG(CH0RES);    /* per-conversion result    */
    DMA0DST = (uint32_t)buf;                /* RAM destination          */
    DMA0CNT = SAMPLES_PER_BUF;              /* transactions per block   */
    DMA0STAT = 0u;                          /* clear any stale flags    */

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

    /* Block-complete interrupt: IRQ 77, IEC2/IFS2 bit 13, IPC9 default
     * priority 4. Nothing fires until the first burst is started. */
    IFS2bits.DMA0IF = 0;
    IEC2bits.DMA0IE = 1;
    console_puts("[dma] channel 0 armed, window 0x4000..0x13FFF, IRQ on\r\n");
}

bool dma0_enabled(void)
{
    return DMA0CHbits.CHEN != 0u;
}

/* ------------------------------------------------------------------ *
 * DMA interrupt - one per buffer half
 *
 * HALF: the first half is complete, the DMA is filling the second.
 * DONE: the second half is complete, the DMA has reloaded to the start
 *       and the ADC burst has ended - apply a pending input change and,
 *       if the stream is enabled, start the next burst here.
 *
 * Priority 4 (IPC9 default). The console's UART receive interrupt runs
 * at priority 1, so this ISR preempts a running command.
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
        if (switch_pending) {
            ADCREG(CH0CON1bits).PINSEL = pinsel_next;
            ADCREG(CH0CON1bits).SAMC   = samc_next;
            pinsel_cur = pinsel_next;
            samc_cur   = samc_next;
            switch_pending = false;
        }
        if (run_enabled) {
            adc_start_burst();        /* next SAMPLES_PER_BUF samples   */
        } else {
            burst_active = false;
        }
    }

    IFS2bits.DMA0IF = 0;
}

/* ------------------------------------------------------------------ *
 * Control API (adc_dma_40msps.h)
 * ------------------------------------------------------------------ */
void capture_start(void)
{
    run_enabled = true;
    if (!burst_active) {
        adc_start_burst();
    }
}

void capture_stop(void)
{
    run_enabled = false;
}

bool capture_running(void)
{
    return run_enabled;
}

bool capture_set_input(uint8_t pinsel, uint8_t samc)
{
    if ((pinsel > 15u) || (samc > 31u)) {
        return false;
    }
    pinsel_next = pinsel;
    samc_next   = samc;
    switch_pending = true;
    if (!burst_active) {
        /* Nothing running: apply right away, the channel is idle. */
        ADCREG(CH0CON1bits).PINSEL = pinsel;
        ADCREG(CH0CON1bits).SAMC   = samc;
        pinsel_cur = pinsel;
        samc_cur   = samc;
        switch_pending = false;
    }
    return true;
}

uint8_t capture_pinsel(void) { return pinsel_cur; }
uint8_t capture_samc(void)   { return samc_cur; }

const volatile uint16_t *capture_completed_half(void)
{
    return &buf[ready_half ? SAMPLES_PER_HALF : 0u];
}

void counters_clear(void)
{
    dma_overrun = 0; dma_addr_err = 0; dma_bus_err = 0;
    late_service = 0; proc_missed = 0;
}

void led_init(void)
{
    LED_OFF();
    LED_TRIS = 0u;
}

void led_mode(uint8_t mode)
{
    led_auto = mode;
    if (mode == 0u)      { LED_OFF(); }
    else if (mode == 1u) { LED_ON();  }
}

uint8_t led_get_mode(void) { return led_auto; }

/* ------------------------------------------------------------------ *
 * Process one completed buffer half
 *
 * Placeholder for the customer's "+ and -" arithmetic. Written as a
 * plain accumulate so the cost of touching every sample is visible in
 * the measurement: at 40 MSPS this loop sees 40 million values per
 * second and per channel, and whether the CPU keeps up is as much a
 * question as the DMA bandwidth.
 * ------------------------------------------------------------------ */
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

bool capture_service(void)
{
    const uint32_t done = blocks_done;
    if (done == seen_blocks) {
        return false;
    }
    if ((done - seen_blocks) > 1u) {
        proc_missed += (done - seen_blocks) - 1u;
    }
    seen_blocks = done;
    process_buffer(capture_completed_half(), SAMPLES_PER_HALF);

    /* Heartbeat: slow while clean, fast once any error counter moved. */
    if (led_auto == 2u) {
        const bool clean = (dma_overrun | dma_addr_err | dma_bus_err |
                            late_service | proc_missed) == 0u;
        if ((done % (clean ? HEARTBEAT_OK : HEARTBEAT_ERR)) == 0u) {
            LED_TOGGLE();
        }
    }
    return true;
}

/* Wait until blocks_done passes a value. Returns 0, or 6 (nothing
 * moves) or 8 (the DMA switched itself off, e.g. on an address fault). */
static uint32_t wait_for_blocks(uint32_t target)
{
    uint32_t n = WAIT_LIMIT;
    while (blocks_done < target) {
        if (DMA0CHbits.CHEN == 0u) { return 8u; }
        if (--n == 0u)             { return 6u; }
    }
    return 0u;
}

uint32_t capture_selftest(uint32_t *mean)
{
    const uint8_t  keep_pinsel = pinsel_cur;
    const uint8_t  keep_samc   = samc_cur;
    const bool     was_running = run_enabled;
    uint32_t       rc;

    (void)capture_set_input(SELFTEST_PINSEL, SELFTEST_SAMC);
    capture_start();

    /* The switch takes effect at the next DONE, then a full burst runs
     * on the new input: wait long enough that the half we judge is the
     * reference and nothing else. */
    rc = wait_for_blocks(blocks_done + SELFTEST_HALVES);
    if (rc == 0u) {
        const uint32_t m = half_mean(capture_completed_half(), SAMPLES_PER_HALF);
        selftest_mean = m;
        if (mean != NULL) { *mean = m; }
        if ((m < SELFTEST_MIN) || (m > SELFTEST_MAX)) { rc = 7u; }
    }

    if (rc == 0u) {
        console_kv("[selftest] mean on internal 15/16 VDD (expect ~3840)", selftest_mean);
    } else if (rc == 6u) {
        console_puts("[selftest] no DMA blocks arrived\r\n");
    } else if (rc == 7u) {
        console_kv("[selftest] mean outside 3648..4032", selftest_mean);
    } else {
        console_puts("[selftest] DMA channel disabled\r\n");
    }

    (void)capture_set_input(keep_pinsel, keep_samc);
    if (rc == 0u) {
        rc = wait_for_blocks(blocks_done + SELFTEST_HALVES);   /* settle */
    }
    if (!was_running) {
        capture_stop();
    }
    return rc;
}

/* ------------------------------------------------------------------ *
 * Register dump - what Part 4 of docs/TROUBLESHOOTING.md asks for
 * ------------------------------------------------------------------ */
void regs_dump(void)
{
    console_puts("[regs] clock\r\n");
    console_kv_hex("OSCCTRL", OSCCTRL);
    console_kv_hex("PLL1CON", PLL1CON);
    console_kv_hex("PLL1DIV", PLL1DIV);
    console_kv_hex("PLL2CON", PLL2CON);
    console_kv_hex("PLL2DIV", PLL2DIV);
    console_kv_hex("CLK1CON", CLK1CON);
    console_kv_hex("CLK1DIV", CLK1DIV);
    console_kv_hex("CLK6CON", CLK6CON);
    console_kv_hex("CLK6DIV", CLK6DIV);
    console_puts("[regs] adc\r\n");
    console_kv_hex("ADxCON", ADCREG(CON));
    console_kv_hex("ADxSTAT", ADCREG(STAT));
    console_kv_hex("ADxCH0CON1", ADCREG(CH0CON1));
    console_kv_hex("ADxCH0CNT", ADCREG(CH0CNT));
    console_kv_hex("ADxCH0RES", ADCREG(CH0RES));
    console_kv_hex("ADxCH0DATA", ADCREG(CH0DATA));
    console_puts("[regs] dma\r\n");
    console_kv_hex("DMACON", DMACON);
    console_kv_hex("DMALOW", DMALOW);
    console_kv_hex("DMAHIGH", DMAHIGH);
    console_kv_hex("DMA0CH", DMA0CH);
    console_kv_hex("DMA0SEL", DMA0SEL);
    console_kv_hex("DMA0STAT", DMA0STAT);
    console_kv_hex("DMA0SRC", DMA0SRC);
    console_kv_hex("DMA0DST", DMA0DST);
    console_kv_hex("DMA0CNT", DMA0CNT);
    console_puts("[regs] interrupts, uart\r\n");
    console_kv_hex("IEC2", IEC2);
    console_kv_hex("IFS2", IFS2);
    console_kv_hex("IPC9", IPC9);
    console_kv_hex("INTCON1", INTCON1);
    console_kv_hex("U1CON", U1CON);
    console_kv_hex("U1STAT", U1STAT);
    console_kv_hex("U1BRG", U1BRG);
    console_puts("[regs] counters\r\n");
    console_kv("blocks_done", blocks_done);
    console_kv("dma_overrun", dma_overrun);
    console_kv("late_service", late_service);
    console_kv("proc_missed", proc_missed);
    console_kv("dma_addr_err", dma_addr_err);
    console_kv("dma_bus_err", dma_bus_err);
    console_kv("selftest_mean", selftest_mean);
    console_kv("fail_code", fail_code);
}

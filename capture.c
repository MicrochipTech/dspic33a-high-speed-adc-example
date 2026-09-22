/*
 * capture.c
 *
 * The measurement of the ADC/DMA example: DMA channel 0 from the ADC
 * result into a double buffer, the DMA interrupt that counts every error
 * the hardware can report and restarts the burst, the start/stop/input
 * control the console uses, the self-test on the internal reference and
 * the per-half processing with the LED heartbeat.
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
 *   restarted. Tying CNT to the DMA buffer keeps ADC and DMA in step -
 *   and is why the burst restart sits in the DMA interrupt, not in adc.c.
 *
 * Self-test
 *   Before the external input is used, the same chain samples the ADC's
 *   internal 15/16 * VDD reference (ADxAN6, DS70005591D Table 16-2) and
 *   checks that the mean of a buffer half is where it must be (~3840).
 *   That proves clock, ADC, DMA and ISR together without a signal source.
 *
 * Every register write below cites the datasheet table or page it comes
 * from. Nothing here has run on silicon - see README.md.
 */

#include <xc.h>
#include "board.h"
#include "adc.h"
#include "capture.h"
#include "led.h"
#include "console.h"
#include "diag.h"

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

static uint32_t         seen_blocks    = 0;

/* A burst is in flight from here until the DMA DONE interrupt. */
static void start_burst(void)
{
    burst_active = true;
    adc_start_burst();
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

/* Switch the stream off hard, so that a 40 MSPS stream does not keep
 * hammering the bus while fail() or a trap handler prints. */
void capture_halt(void)
{
    IEC2bits.DMA0IE = 0;
    DMA0CHbits.CHEN = 0;
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
            adc_set_input(pinsel_next, samc_next);
            switch_pending = false;
        }
        if (run_enabled) {
            start_burst();            /* next SAMPLES_PER_BUF samples   */
        } else {
            burst_active = false;
        }
    }

    IFS2bits.DMA0IF = 0;
}

/* ------------------------------------------------------------------ *
 * Control API (capture.h)
 * ------------------------------------------------------------------ */
void capture_start(void)
{
    run_enabled = true;
    if (!burst_active) {
        start_burst();
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
        adc_set_input(pinsel, samc);
        switch_pending = false;
    }
    return true;
}

uint8_t capture_pinsel(void) { return adc_pinsel(); }
uint8_t capture_samc(void)   { return adc_samc(); }

const volatile uint16_t *capture_completed_half(void)
{
    return &buf[ready_half ? SAMPLES_PER_HALF : 0u];
}

void counters_clear(void)
{
    dma_overrun = 0; dma_addr_err = 0; dma_bus_err = 0;
    late_service = 0; proc_missed = 0;
    /* Halves completed up to now are not "missed" from here on. Without
     * this the first capture_service() after the self-test books its
     * 12 unserviced halves as proc_missed, and the heartbeat goes to the
     * error rate before the measurement has even started. */
    seen_blocks = blocks_done;
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
    if (led_get_mode() == 2u) {
        const bool clean = (dma_overrun | dma_addr_err | dma_bus_err |
                            late_service | proc_missed) == 0u;
        if ((done % (clean ? HEARTBEAT_OK : HEARTBEAT_ERR)) == 0u) {
            led_toggle();
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
    const uint8_t  keep_pinsel = adc_pinsel();
    const uint8_t  keep_samc   = adc_samc();
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

void capture_regs_dump(void)
{
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
    console_kv_hex("IEC2", IEC2);           /* DMA0 enable,  bit 13      */
    console_kv_hex("IFS2", IFS2);           /* DMA0 flag,    bit 13      */
    console_kv_hex("IPC9", IPC9);           /* DMA0 priority             */
    console_puts("[regs] counters\r\n");
    console_kv("blocks_done", blocks_done);
    console_kv("dma_overrun", dma_overrun);
    console_kv("late_service", late_service);
    console_kv("proc_missed", proc_missed);
    console_kv("dma_addr_err", dma_addr_err);
    console_kv("dma_bus_err", dma_bus_err);
    console_kv("selftest_mean", selftest_mean);
}

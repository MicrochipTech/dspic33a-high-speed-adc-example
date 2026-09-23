/*
 * capture.c
 *
 * The measurement of the ADC/DMA example: the ADC result streams through
 * DMA channel 0 (dma.c) into a double buffer; the channel's events are
 * handled here - every error the hardware can report is counted and the
 * burst is restarted - plus the start/stop/input control the console
 * uses, the self-test on the internal reference and the per-half
 * processing with the LED heartbeat.
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
 * from. What has and has not run on the board: docs/HARDWARE-LOG.md.
 */

#include <xc.h>
#include "board.h"
#include "adc.h"
#include "dma.h"
#include "capture.h"
#include "led.h"
#include "console.h"
#include "diag.h"
#include "sim.h"
#include "timebase.h"

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
/* Guard words directly behind the buffer, in the same struct so that the
 * linker cannot put anything between. capture_init() fills them with a
 * pattern, capture_service() and the self-test check them: if the DMA
 * writes one transaction past the buffer - wrong SIZE encoding, wrong
 * CNT semantics, wrong DAMODE - this is the first memory it hits, and
 * the run stops in fail(11) with the words printed instead of dying
 * somewhere in the variables or the stack that follow. */
#define BUF_GUARD_WORDS       16u
#define BUF_GUARD_PATTERN(i)  (0xA5C3F00Du + (i))

/* THE DMA BUFFER - a dedicated object, and nothing else is in it.
 *
 *   volatile   the DMA writes it behind the compiler's back; every read
 *              must go to memory, never to a cached register value
 *   section    its own section ".dma_buffer", so the linker keeps it in
 *              one piece and no other variable is laid out inside or
 *              across it (see the map file)
 *   aligned    4 bytes, the DMA writes through a 32-bit path
 *   window     dma0_init() sets DMALOW/DMAHIGH to exactly this object:
 *              the hardware refuses any DMA transaction outside it
 *   guard      the words behind the samples are the software check of
 *              the same thing (fail 11)
 *
 * So a DMA transfer cannot collide with the rest of memory: not by
 * layout, not by the hardware, and if it somehow did, not unnoticed. */
static volatile struct {
    uint16_t data[SAMPLES_PER_BUF];
    uint32_t guard[BUF_GUARD_WORDS];
} dma_buffer __attribute__((section(".dma_buffer"), aligned(4)));
#define buf (dma_buffer.data)

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
static volatile bool    period_pending = false;
static volatile uint8_t period_next    = ADC_RPTCNT;

static uint32_t         seen_blocks    = 0;

/* A burst is in flight from here until the DMA DONE interrupt. */
static void start_burst(void)
{
    burst_active = true;
    adc_start_burst();
}

/* ------------------------------------------------------------------ *
 * Set-up: the DMA channel, wired to this ADC core and this buffer
 * ------------------------------------------------------------------ */
void capture_init(void)
{
    for (uint32_t i = 0; i < BUF_GUARD_WORDS; i++) {
        dma_buffer.guard[i] = BUF_GUARD_PATTERN(i);
    }
    /* The buffer object itself and its size - not a constant that has to
     * agree with it. The ADC's burst length (adc_init, SAMPLES_PER_BUF)
     * must equal the block, which the check below pins down. */
    dma0_init(DMA_TRIG_ADC_CH0, &ADCREG(CH0RES), dma_buffer.data, sizeof dma_buffer.data);
}
_Static_assert(sizeof dma_buffer.data == SAMPLES_PER_BUF * sizeof(uint16_t),
               "ADC burst length and DMA buffer size must be the same thing");

/* Stop with code 11 if anything wrote past the end of the buffer. */
static void guard_check(void)
{
    for (uint32_t i = 0; i < BUF_GUARD_WORDS; i++) {
        if (dma_buffer.guard[i] != BUF_GUARD_PATTERN(i)) {
            console_kv("[guard] word behind the buffer changed, index", i);
            console_kv_hex("[guard] value", dma_buffer.guard[i]);
            console_kv_hex("[guard] expected", BUF_GUARD_PATTERN(i));
            fail(11u);
        }
    }
}

/* Switch the stream off hard, so that a 40 MSPS stream does not keep
 * hammering the bus while fail() or a trap handler prints. */
void capture_halt(void)
{
    dma0_halt();
}

/* ------------------------------------------------------------------ *
 * DMA event - one per buffer half, called from the DMA0 interrupt
 *
 * HALF: the first half is complete, the DMA is filling the second.
 * DONE: the second half is complete, the DMA has reloaded to the start
 *       and the ADC burst has ended - apply a pending input change and,
 *       if the stream is enabled, start the next burst here.
 *
 * Runs in the interrupt, priority 4 (IPC9 default). The console's UART
 * receive interrupt runs at priority 1, so this preempts a running
 * command.
 *
 * Deliberately short. Everything this counts is a hardware flag, so a
 * long handler would itself become the reason for the next overrun.
 * ------------------------------------------------------------------ */
void dma0_event(uint32_t st)
{
    if (st & DMA0_OVERRUN) {
        /* Triggered while the previous transfer was still in progress
         * (p816): the bus did not keep up. This is the measurement. */
        dma_overrun++;
        dma0_clear(DMA0_OVERRUN);
    }
    if (st & DMA0_ADRERR) {
        dma_addr_err++;
        dma0_clear(DMA0_ADRERR);
    }
    if (st & (DMA0_BRERR | DMA0_BWERR)) {
        dma_bus_err++;
        dma0_clear(DMA0_BRERR | DMA0_BWERR);
    }

    /* Both halves pending at once means this handler arrived more than
     * one half (25.6 us) late and the first half has already been
     * overwritten by the DMA reload. */
    if ((st & DMA0_HALF) && (st & DMA0_DONE)) {
        late_service++;
    }

    if (st & DMA0_HALF) {
        dma0_clear(DMA0_HALF);
        ready_half  = 0u;
        last_sample = buf[SAMPLES_PER_HALF - 1u];
        blocks_done++;
    }
    if (st & DMA0_DONE) {
        dma0_clear(DMA0_DONE);
        ready_half  = 1u;
        last_sample = buf[SAMPLES_PER_BUF - 1u];
        blocks_done++;

        /* The channel is idle between bursts: this is the only safe
         * moment to change its input or sample time. */
        if (switch_pending) {
            adc_set_input(pinsel_next, samc_next);
            switch_pending = false;
        }
        if (period_pending) {
            adc_set_period(period_next);
            period_pending = false;
        }
        if (run_enabled) {
            start_burst();            /* next SAMPLES_PER_BUF samples   */
        } else {
            burst_active = false;
        }
    }
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

bool capture_burst_active(void)
{
    return burst_active;
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

bool capture_set_period(uint8_t rptcnt)
{
    if ((rptcnt < 2u) || (rptcnt > 63u)) {
        return false;
    }
    period_next    = rptcnt;
    period_pending = true;
    if (!burst_active) {
        adc_set_period(rptcnt);
        period_pending = false;
    }
    return true;
}

uint8_t capture_period(void) { return adc_period(); }

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
    SIM_CHECK_HALF(b, n);             /* simulator: is this really the next half? */
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
    guard_check();                    /* did the DMA stay inside buf?    */

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
        SIM_DMA_TICK();               /* simulator: deliver a half     */
        if (!dma0_enabled()) { return 8u; }
        if (--n == 0u)       { return 6u; }
    }
    guard_check();
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

/* ------------------------------------------------------------------ *
 * Rate self-test
 *
 * The ADC's repeat timer is what sets the sample rate; Timer1
 * (timebase.c) is only the stopwatch that checks it. Run RATETEST_HALVES
 * halves at RPTCNT 16 and at RPTCNT 4, count ticks, compare the delivered
 * rate with the nominal 80000 / RPTCNT kSPS. Then the ratio of the two:
 * the failure seen on the board was a rate that did not move when the
 * period changed, and that is caught even if the nominal figure itself
 * were off by a constant factor (RPTCNT vs RPTCNT + 1 cycles, or a wrong
 * TAD). The burst restart from the DMA interrupt costs a little per
 * 2048 samples, which the 10 % tolerance covers.
 * ------------------------------------------------------------------ */
#define RATETEST_HALVES   200u
#define RATETEST_TOL_PCT  10u

#ifndef __MPLAB_DEBUGGER_SIMULATOR
static uint32_t rate_measure(uint8_t rptcnt, uint32_t *ksps)
{
    uint32_t n = WAIT_LIMIT;
    capture_stop();
    while (burst_active && (--n != 0u)) { SIM_DMA_TICK(); }
    if (n == 0u) { return 6u; }
    (void)capture_set_period(rptcnt);            /* idle: applied now   */
    counters_clear();
    const uint32_t target = blocks_done + RATETEST_HALVES;
    const uint32_t t0     = timebase_ticks();
    capture_start();
    const uint32_t rc     = wait_for_blocks(target);
    const uint32_t ticks  = timebase_ticks() - t0;
    capture_stop();
    if (rc != 0u) { return rc; }
    *ksps = timebase_ksps(RATETEST_HALVES * SAMPLES_PER_HALF, ticks);
    return 0u;
}
#endif

uint32_t capture_ratetest(void)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    console_puts("[ratetest] skipped: the simulator has no ADC clock to measure\r\n");
    return 0u;
#else
    static const uint8_t rpt[2] = { 16u, 4u };   /* 5 and 20 MSPS nominal */
    const uint8_t keep        = capture_period();
    const bool    was_running = capture_running();
    uint32_t      ksps[2]     = { 0u, 0u };
    uint32_t      rc          = 0u;

    timebase_init();
    console_kv("[ratetest] time base check, ticks per 100 ms (expect 1250000)", timebase_check());

    for (uint32_t i = 0; (i < 2u) && (rc == 0u); i++) {
        rc = rate_measure(rpt[i], &ksps[i]);
        if (rc == 0u) {
            const uint32_t nominal = 80000u / rpt[i];
            const uint32_t diff    = (ksps[i] > nominal) ? ksps[i] - nominal : nominal - ksps[i];
            console_kv("[ratetest] rptcnt", rpt[i]);
            console_kv("[ratetest]   nominal ksps", nominal);
            console_kv("[ratetest]   measured ksps", ksps[i]);
            if (diff > nominal * RATETEST_TOL_PCT / 100u) {
                console_puts("[ratetest]   outside the 10 % window\r\n");
                rc = 12u;
            }
        }
    }
    if (rc == 0u) {
        /* RPTCNT 16 -> 4 must make the rate four times higher (3..5x). */
        if ((ksps[1] < 3u * ksps[0]) || (ksps[1] > 5u * ksps[0])) {
            console_puts("[ratetest] the rate does not follow the period\r\n");
            rc = 12u;
        }
    }

    (void)capture_set_period(keep);
    counters_clear();
    if (was_running) { capture_start(); }
    console_puts((rc == 0u) ? "[ratetest] passed: the rate follows RPTCNT\r\n"
                            : "[ratetest] FAILED\r\n");
    return rc;
#endif
}

void capture_regs_dump(void)
{
    console_puts("[regs] counters\r\n");
    console_kv("blocks_done", blocks_done);
    console_kv("dma_overrun", dma_overrun);
    console_kv("late_service", late_service);
    console_kv("proc_missed", proc_missed);
    console_kv("dma_addr_err", dma_addr_err);
    console_kv("dma_bus_err", dma_bus_err);
    console_kv("selftest_mean", selftest_mean);
    /* The guard words behind the buffer: all must read 0xA5C3F00D + i. */
    console_kv_hex("buf", (uint32_t)buf);
    console_kv_hex("buf_end", (uint32_t)&buf[SAMPLES_PER_BUF]);
    console_kv_hex("guard0", dma_buffer.guard[0]);
    console_kv_hex("guard1", dma_buffer.guard[1]);
    console_kv_hex("guard15", dma_buffer.guard[BUF_GUARD_WORDS - 1u]);
}

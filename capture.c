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
#include <stddef.h>
#include "board.h"
#include "adc.h"
#include "dma.h"
#include "capture.h"
#include "led.h"
#include "console.h"
#include "diag.h"
#include "sim.h"
#include "timebase.h"
#include "clock.h"

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
    uint16_t data[SAMPLES_PER_BUF_MAX];
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
static volatile bool    powered      = true;    /* ADC core + CLKGEN6 on */
static volatile bool    dma_armed    = false;   /* dma0_init() done, not deinit */
static volatile uint32_t half_len    = SAMPLES_PER_HALF_MAX;   /* in use  */

/* Channel reconfiguration requested by the console or the self-test,
 * applied by the ISR between two bursts, when the channel is idle. */
static volatile bool    switch_pending = false;
static volatile uint8_t pinsel_next    = ADC_PINSEL;
static volatile uint8_t samc_next      = ADC_SAMC;
/* The ADC clock divide ratio in hundredths, as last set successfully.
 * The hardware's own answer is clock_adc_div(); this is what was asked
 * for, so that a switch that did not arrive can be told from one that
 * did (capture_clkdiv_wanted vs capture_clkdiv). */
static uint32_t          clkdiv_cur     = ADC_CLKDIV;

/* capture.h: the defined idle state every test starts from. */
bool capture_settle(void)
{
    const bool was_running = run_enabled;
    capture_stop();
    /* The burst in flight ends at a block boundary by itself - at most
     * one buffer, 52 us at 40 MSPS. One that never ends (no clock, no
     * trigger) is aborted: core down and up. */
    uint32_t n = WAIT_LIMIT;
    while (burst_active && (--n != 0u)) { SIM_DMA_TICK(); }
    if (burst_active) {
        if (powered) { adc_deinit(); (void)adc_reinit(); }
        burst_active = false;
    }
    if (powered) { adc_clear_events(); }
    dma0_deinit();                    /* channel down; capture_start() */
    dma_armed  = false;               /* re-initialises it from scratch */
    ready_half = 0u;
    return was_running;
}
static bool quiesce(void) { return capture_settle(); }

static uint32_t         seen_blocks    = 0;

/* A burst is in flight from here until the DMA DONE interrupt. */
static void start_burst(void)
{
    burst_active = true;
    {
        adc_start_burst();
    }
}

/* ------------------------------------------------------------------ *
 * Set-up: the DMA channel, wired to this ADC core and this buffer
 * ------------------------------------------------------------------ */
/* The guard words sit right behind the region in use: the struct's own
 * guard when the whole array is used, otherwise the words of the array
 * that follow the used region (a DMA writing one transaction too many
 * hits them first either way). 16 words = 8 samples' worth of 32-bit
 * words = 32 samples; the minimum half length keeps room for them. */
static volatile uint32_t *guard_word(uint32_t i)
{
    const uint32_t used = 2u * half_len;
    if (used + 2u * BUF_GUARD_WORDS <= SAMPLES_PER_BUF_MAX) {
        return (volatile uint32_t *)&dma_buffer.data[used] + i;
    }
    return &dma_buffer.guard[i];
}

void capture_init(void)
{
    for (uint32_t i = 0; i < BUF_GUARD_WORDS; i++) {
        dma_buffer.guard[i] = BUF_GUARD_PATTERN(i);
        *guard_word(i)      = BUF_GUARD_PATTERN(i);
    }
    /* Burst length and DMA block are the same number, set here from the
     * length in use: 2 * half_len conversions per burst, 2 * half_len
     * transactions per block, both re-done before every start. */
    adc_set_burst_len(2u * half_len);
    dma0_init(adc_dma_trigger(), adc_dma_source(), dma_buffer.data, 2u * half_len * sizeof(uint16_t));
    dma_armed = true;
}
_Static_assert(sizeof dma_buffer.data == SAMPLES_PER_BUF_MAX * sizeof(uint16_t),
               "buffer allocation and its maximum must be the same thing");

uint32_t capture_half_len(void) { return half_len; }

bool capture_set_half_len(uint32_t n)
{
    if ((n < SAMPLES_PER_HALF_MIN) || (n > SAMPLES_PER_HALF_MAX)) { return false; }
    if (run_enabled || burst_active) { return false; }      /* stop first */
    (void)capture_settle();           /* DMA down; the next start re-inits */
    half_len = n;
    return true;
}

/* Stop with code 11 if anything wrote past the end of the buffer. */
static void guard_check(void)
{
    for (uint32_t i = 0; i < BUF_GUARD_WORDS; i++) {
        if (*guard_word(i) != BUF_GUARD_PATTERN(i)) {
            console_kv("[guard] word behind the buffer changed, index", i);
            console_kv_hex("[guard] value", *guard_word(i));
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
        last_sample = buf[half_len - 1u];
        blocks_done++;
    }
    if (st & DMA0_DONE) {
        dma0_clear(DMA0_DONE);
        ready_half  = 1u;
        last_sample = buf[2u * half_len - 1u];
        blocks_done++;

        /* The channel is idle between bursts: this is the only safe
         * moment to change its input or sample time. */
        if (switch_pending) {
            adc_set_input(pinsel_next, samc_next);
            switch_pending = false;
        }
        if (run_enabled) {
            start_burst();            /* next 2 * half_len samples      */
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
    if (!powered) {
        /* After capture_shutdown(): clock first, then the core - the boot
         * order. Registers kept their values, so the divide ratio is
         * what they were. A wait that runs out is reported and the
         * start refused; the counters show nothing moving. */
        const bool clk = clock_adc_on();
        const bool adc = adc_reinit();
        if (!clk || !adc) {
            console_puts(clk ? "[capture] ADC core did not come back (ADRDY)\r\n"
                             : "[capture] CLKGEN6 did not come back (CLKRDY)\r\n");
            return;
        }
        powered = true;
    }
    if (!dma_armed) {
        capture_init();               /* fresh DMA set-up for this run  */
    }
    run_enabled = true;
    if (!burst_active) {
        start_burst();
    }
}

void capture_shutdown(void)
{
    (void)quiesce();                  /* stream off, burst finished     */
    adc_deinit();
    clock_adc_off();
    powered = false;
}

bool capture_powered(void)
{
    return powered;
}

bool capture_select_core(uint8_t core, uint8_t pinsel, uint8_t samc)
{
    if ((core < 1u) || (core > 5u)) { return false; }
    (void)quiesce();
    if (powered) { adc_deinit(); }
    else         { (void)clock_adc_on(); }   /* the new core needs its clock */
    (void)adc_select(core);
    adc_init(pinsel, samc);                  /* core on, ADRDY, or fail(5) */
    capture_init();                          /* DMA on this core's trigger */
    switch_pending = false;
    pinsel_next    = pinsel;
    samc_next      = samc;
    powered        = true;
    burst_active   = false;
    counters_clear();
    return true;
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

/* ------------------------------------------------------------------ *
 * The ADC clock - the only rate control there is (capture.h)
 *
 * The conversions run back-to-back, so the sample rate is the ADC clock
 * divided by the eight clocks one conversion takes: 320 MHz undivided =
 * 40 MSPS. CLKGEN6's divider is therefore the rate knob, and the ladder
 * below is walked from the slowest rate to the fastest on purpose. The
 * slowest point is the one the DMA should manage comfortably, so the
 * first row of a sweep is the row most likely to pass - and a failure
 * there means the chain is broken, not the rate.
 *
 * Integer and fractional ratios alternate deliberately. 2.5 sits exactly
 * between 2 and 3, and 4.5 between 4 and 5: if those rows land halfway
 * between their neighbours, FRACDIV works; if they snap to a neighbour,
 * the fractional field is ignored and only INTDIV counts. Nothing else
 * in the log answers that question.
 *
 * 500 = 8 MSPS is the rate the customer's application needs; it is in
 * the ladder for that reason and for no other.
 * ------------------------------------------------------------------ */
static const uint32_t sweep_ratios[] = {
    1000u,  /* /10   32.0 MHz   4.00 MSPS  slowest the ADC may run       */
     900u,  /* /9    35.6 MHz   4.44 MSPS                                */
     800u,  /* /8    40.0 MHz   5.00 MSPS                                */
     700u,  /* /7    45.7 MHz   5.71 MSPS                                */
     600u,  /* /6    53.3 MHz   6.67 MSPS                                */
     500u,  /* /5    64.0 MHz   8.00 MSPS  the customer's floor          */
     450u,  /* /4.5  71.1 MHz   8.89 MSPS  fractional, between 4 and 5   */
     400u,  /* /4    80.0 MHz  10.00 MSPS                                */
     350u,  /* /3.5  91.4 MHz  11.43 MSPS  fractional                    */
     300u,  /* /3   106.7 MHz  13.33 MSPS                                */
     250u,  /* /2.5 128.0 MHz  16.00 MSPS  fractional, between 2 and 3   */
     200u,  /* /2   160.0 MHz  20.00 MSPS                                */
     150u,  /* /1.5 213.3 MHz  26.67 MSPS  INTDIV 0, fraction alone      */
     125u,  /* /1.25 256.0 MHz 32.00 MSPS  INTDIV 0, fraction alone      */
     100u   /* /1   320.0 MHz  40.00 MSPS  undivided                     */
};

uint32_t capture_set_clkdiv(uint32_t ratio_h)
{
    /* The order the user asked for, and it is the boot order run
     * backwards and forwards again: DMA channel down and ADC core off
     * (capture_settle, adc_deinit), CLKGEN6 off, divider written, read
     * back, generator on, DIVSWEN and CLKRDY awaited, fields read back
     * once more (clock_adc_set_div), core on and ADRDY awaited
     * (adc_reinit), DMA set up from scratch on the next capture_start().
     * Nothing is reconfigured under a running clock or a running core. */
    const bool restart = capture_settle();      /* DMA down, burst ended */
    adc_deinit();                               /* core off              */
    const uint32_t rc    = clock_adc_set_div(ratio_h);
    const bool     ready = adc_reinit();        /* core on, ADRDY        */
    if (rc == CLKDIV_OK) { clkdiv_cur = ratio_h; }
    if (restart) { capture_start(); }           /* dma0_init() again     */
    if (rc != CLKDIV_OK) { return rc; }
    return ready ? CLKDIV_OK : CLKDIV_ADC;
}

uint32_t capture_clkdiv(void)        { return clock_adc_div(); }  /* hardware */
uint32_t capture_clkdiv_wanted(void) { return clkdiv_cur; }       /* asked for */

uint32_t capture_nominal_ksps(uint32_t ratio_h)
{
    /* 40 MSPS at ratio 1, and the ratio is in hundredths:
     * 40 000 ksps * 100 / ratio_h. */
    return (ratio_h == 0u) ? 0u : (4000000u / ratio_h);
}

const uint32_t *capture_sweep_ratios(uint32_t *count)
{
    *count = sizeof sweep_ratios / sizeof sweep_ratios[0];
    return sweep_ratios;
}

const volatile uint16_t *capture_completed_half(void)
{
    return &buf[ready_half ? half_len : 0u];
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
    process_buffer(capture_completed_half(), half_len);
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
    const bool     was_running = capture_settle();   /* defined start   */
    uint32_t       rc;

    (void)capture_set_input(SELFTEST_PINSEL, SELFTEST_SAMC);
    capture_start();

    /* The switch takes effect at the next DONE, then a full burst runs
     * on the new input: wait long enough that the half we judge is the
     * reference and nothing else. */
    rc = wait_for_blocks(blocks_done + SELFTEST_HALVES);
    if (rc == 0u) {
        const uint32_t m = half_mean(capture_completed_half(), half_len);
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
 * Delivered rate
 *
 * Timer1 (timebase.c) is only the stopwatch; the ADC clock produces the
 * rate. Measuring it is one thing and one thing only now: run `halves`
 * halves at whatever the divider is set to and divide the sample count
 * by the elapsed time. The judgement - does this rate match the ratio,
 * and did anything get lost - belongs to the caller, which prints it.
 * ------------------------------------------------------------------ */
uint32_t capture_measure_rate(uint32_t halves, uint32_t *ksps)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    (void)halves;
    if (ksps != NULL) { *ksps = 0u; }
    return 0u;                        /* no ADC clock to measure        */
#else
    (void)capture_settle();           /* defined start                  */
    counters_clear();
    const uint32_t target = blocks_done + halves;
    const uint32_t t0     = timebase_ticks();
    capture_start();
    const uint32_t rc     = wait_for_blocks(target);
    const uint32_t ticks  = timebase_ticks() - t0;
    (void)capture_settle();           /* test over: DMA down            */
    if (rc != 0u) { return rc; }
    if (ksps != NULL) { *ksps = timebase_ksps(halves * half_len, ticks); }
    return 0u;
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
    console_kv("half_len", half_len);
    console_kv_hex("buf_end (in use)", (uint32_t)&buf[2u * half_len]);
    console_kv_hex("guard0", dma_buffer.guard[0]);
    console_kv_hex("guard1", dma_buffer.guard[1]);
    console_kv_hex("guard15", dma_buffer.guard[BUF_GUARD_WORDS - 1u]);
}

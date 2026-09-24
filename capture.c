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
#include "sccp.h"
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
static volatile bool    powered      = true;    /* ADC core + CLKGEN6 on */
static volatile bool    dma_armed    = false;   /* dma0_init() done, not deinit */

/* Channel reconfiguration requested by the console or the self-test,
 * applied by the ISR between two bursts, when the channel is idle. */
static volatile bool    switch_pending = false;
static volatile uint8_t pinsel_next    = ADC_PINSEL;
static volatile uint8_t samc_next      = ADC_SAMC;
static volatile bool     period_pending = false;
static volatile uint32_t period_next    = ADC_RPTCNT;
static volatile bool     pacing_pending = false;
static volatile uint8_t  pacing_next    = ADC_TRG2_REPEAT;
static uint32_t          sccp_ticks     = ADC_SCCP_TICKS;   /* SCCP1 period */
/* The active pacing. TRG2SRC alone cannot tell the clock-divider source
 * from plain back-to-back: both run the converter back-to-back. */
static volatile uint8_t  pacing_cur     = ADC_TRG2_REPEAT;   /* adc_init() */

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

/* The ADC clock is not changed under a running core: adc_deinit(),
 * CLKGEN6 divider switched, adc_reinit() and ADRDY back - the same
 * order as at boot. Idle only (quiesce() first). False if the switch or
 * the core did not come back; the log says so through the rate test or
 * the sweep row. */
static bool adc_clock_switch(uint32_t ratio)
{
    adc_deinit();
    const bool switched = clock_adc_set_div(ratio);
    const bool ready    = adc_reinit();
    return switched && ready;
}

/* Write the period for the active source; the caller guarantees the
 * channel is idle (or that this is the DONE moment). The clock-divider
 * source never gets here from the DONE moment: its changes go through
 * quiesce() and happen at once, because they switch the ADC off. */
static void apply_period(uint32_t period)
{
    switch (pacing_cur) {
    case ADC_TRG2_REPEAT:  adc_set_period((uint8_t)period); break;
    case ADC_TRG2_SCCP1:   sccp_ticks = period; sccp1_start(period); break;
    case ADC_PACE_SINGLE:  sccp_ticks = period;
                           if (run_enabled) { sccp1_start(period); }
                           break;
    case ADC_PACE_CLKDIV:  (void)adc_clock_switch(period); break;
    default:               break;                /* B2B: nothing to set */
    }
}

static uint32_t         seen_blocks    = 0;

/* A burst is in flight from here until the DMA DONE interrupt. For the
 * single-conversion source "burst" means the SCCP1 trigger is running:
 * conversions come one per trigger until capture_stop() stops it. */
static void start_burst(void)
{
    burst_active = true;
    if (pacing_cur == ADC_PACE_SINGLE) {
        sccp1_start(sccp_ticks);
    } else {
        adc_start_burst();
    }
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
    dma0_init(adc_dma_trigger(), adc_dma_source(), dma_buffer.data, sizeof dma_buffer.data);
    dma_armed = true;
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
        if (pacing_pending) {
            /* Never involves the clock-divider or single-conversion
             * source: those go through quiesce() in capture_set_pacing(). */
            adc_set_trg2((pacing_next == ADC_PACE_CLKDIV) ? ADC_TRG2_B2B : pacing_next);
            pacing_cur     = pacing_next;
            pacing_pending = false;
        }
        if (period_pending) {
            apply_period(period_next);
            period_pending = false;
        }
        if (pacing_cur == ADC_PACE_SINGLE) {
            /* Nothing to restart: the trigger keeps running and the DMA
             * has reloaded its block. capture_stop() ends it. */
        } else if (run_enabled) {
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
    if (!powered) {
        /* After capture_shutdown(): clock first, then the core - the boot
         * order. Registers kept their values, so pacing and period are
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
    sccp1_stop();
    if (powered) { adc_deinit(); }
    else         { (void)clock_adc_on(); }   /* the new core needs its clock */
    if (clock_adc_div() != 100u) { (void)clock_adc_set_div(100u); }
    (void)adc_select(core);
    adc_init(pinsel, samc, ADC_RPTCNT);      /* core on, ADRDY, or fail(5) */
    capture_init();                          /* DMA on this core's trigger */
    pacing_cur     = ADC_TRG2_REPEAT;
    pacing_pending = false;
    period_pending = false;
    switch_pending = false;
    pinsel_next    = pinsel;
    samc_next      = samc;
    sccp_ticks     = ADC_SCCP_TICKS;
    powered        = true;
    burst_active   = false;
    counters_clear();
    return true;
}

void capture_stop(void)
{
    run_enabled = false;
    if (pacing_cur == ADC_PACE_SINGLE) {
        sccp1_stop();                 /* no trigger, no conversion      */
        burst_active = false;
    }
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
 * Pacing (capture.h): which trigger sets the rate, and its period
 * ------------------------------------------------------------------ */
static const uint32_t sweep_repeat[] = { 63u, 32u, 16u, 8u, 4u, 3u, 2u };  /* 1.27..40 MSPS */
static const uint32_t sweep_sccp[]   = { 80u, 40u, 20u, 10u, 8u, 5u, 4u }; /* 1.25..25 MSPS */
static const uint32_t sweep_b2b[]    = { 0u };                             /* one row       */
/* ADC clock divider, ratio in hundredths. First pass the even ratios,
 * INTDIV alone: 10, 8, 6, 4, 2, 1 = 4, 5, 6.7, 10, 20, 40 MSPS (32 MHz is
 * the ADC clock minimum, so 10 is the largest). Second pass with the
 * fractional part: 9, 7, 5, 3, 2.5, 1.6, 1.25 = 4.4, 5.7, 8, 13.3, 16,
 * 25, 32 MSPS - 8 MSPS is the customer's floor, 25 and 32 are the rates
 * the even ratios cannot reach; below 200 INTDIV is 0 and the fraction
 * alone has to divide, which the measured column will confirm or not. */
static const uint32_t sweep_clkdiv[]  = { 1000u, 800u, 600u, 400u, 200u, 100u };
static const uint32_t sweep_clkdiv2[] = { 900u, 700u, 500u, 300u, 250u, 160u, 125u };

bool capture_set_pacing(uint8_t pacing)
{
    uint32_t period;
    switch (pacing) {
    case ADC_TRG2_REPEAT:  period = ADC_RPTCNT;     break;
    case ADC_TRG2_SCCP1:   period = ADC_SCCP_TICKS; break;
    case ADC_PACE_CLKDIV:  period = ADC_CLKDIV;     break;
    case ADC_PACE_SINGLE:  period = ADC_SCCP_TICKS; break;
    case ADC_TRG2_B2B:     period = 0u;             break;
    default:               return false;
    }
    /* Into or out of the clock-divider source the ADC is switched off
     * and on, into or out of the single-conversion source its operating
     * mode changes: not between two bursts in the ISR, but now, with the
     * stream stopped and restarted around it. */
    bool restart = false;
    if ((pacing == ADC_PACE_CLKDIV) || (pacing_cur == ADC_PACE_CLKDIV) ||
        (pacing == ADC_PACE_SINGLE) || (pacing_cur == ADC_PACE_SINGLE)) {
        restart = quiesce();
    }
    if (pacing != ADC_TRG2_SCCP1) { sccp1_stop(); }
    if ((pacing_cur == ADC_PACE_SINGLE) && (pacing != ADC_PACE_SINGLE) && !burst_active) {
        adc_set_mode_burst();
    }
    /* Leaving the clock-divider source: back to the full ADC clock, so
     * that the other sources' units (TAD, 10 ns ticks) mean what the
     * comments say. */
    if ((pacing_cur == ADC_PACE_CLKDIV) && (pacing != ADC_PACE_CLKDIV)) {
        (void)adc_clock_switch(1u);
    }
    pacing_next    = pacing;
    pacing_pending = true;
    period_next    = period;
    period_pending = true;
    if (!burst_active) {
        if (pacing == ADC_PACE_SINGLE) {
            adc_set_mode_single(ADC_TRG2_SCCP1);   /* same code in Table 16-3 */
            adc_set_trg2(ADC_TRG2_B2B);            /* unused in this mode     */
        } else {
            adc_set_trg2((pacing == ADC_PACE_CLKDIV) ? ADC_TRG2_B2B : pacing);
        }
        pacing_cur = pacing;
        apply_period(period);
        pacing_pending = period_pending = false;
    }
    if (restart) { capture_start(); }
    return true;
}

uint8_t capture_pacing(void) { return pacing_cur; }

const char *capture_pacing_name(void)
{
    switch (pacing_cur) {
    case ADC_TRG2_REPEAT:  return "ADC repeat timer (period in TAD = 12.5 ns)";
    case ADC_TRG2_SCCP1:   return "SCCP1 timer (period in ticks of 10 ns)";
    case ADC_PACE_CLKDIV:  return "ADC clock divider (period = divide ratio of 320 MHz, back-to-back)";
    case ADC_PACE_SINGLE:  return "one conversion per SCCP1 trigger, no burst (period in ticks of 10 ns)";
    case ADC_TRG2_B2B:     return "back-to-back (no rate control)";
    default:               return "unknown pacing";
    }
}

bool capture_set_period(uint32_t period)
{
    switch (pacing_cur) {
    case ADC_TRG2_REPEAT:  if ((period < 2u) || (period > 63u))    { return false; } break;
    case ADC_TRG2_SCCP1:   if ((period < 2u) || (period > 65535u)) { return false; } break;
    case ADC_PACE_SINGLE:  if ((period < 2u) || (period > 65535u)) { return false; } break;
    case ADC_PACE_CLKDIV:  if ((period < 100u) || (period > 1000u)) { return false; } break;
    default:               return false;
    }
    if (pacing_cur == ADC_PACE_CLKDIV) {
        /* ADC off and on around the clock switch: stop the stream, do
         * it now, restart. Not deferred to the DONE moment. */
        const bool restart = quiesce();
        const bool ok      = adc_clock_switch(period);
        period_pending     = false;
        if (restart) { capture_start(); }
        return ok;
    }
    period_next    = period;
    period_pending = true;
    if (!burst_active) {
        apply_period(period);
        period_pending = false;
    }
    return true;
}

uint32_t capture_period(void)
{
    switch (pacing_cur) {
    case ADC_TRG2_REPEAT:  return adc_period();
    case ADC_TRG2_SCCP1:   return sccp_ticks;
    case ADC_PACE_SINGLE:  return sccp_ticks;
    case ADC_PACE_CLKDIV:  return clock_adc_div();
    default:               return 0u;
    }
}

uint32_t capture_nominal_ksps(uint32_t period)
{
    if (period == 0u) { return 0u; }
    switch (pacing_cur) {
    case ADC_TRG2_REPEAT:  return 80000u / period;              /* 320 MHz / 4 / n */
    case ADC_TRG2_SCCP1:   return SCCP_TICK_HZ / 1000u / period; /* 100 MHz / n     */
    case ADC_PACE_SINGLE:  return SCCP_TICK_HZ / 1000u / period; /* 100 MHz / n     */
    case ADC_PACE_CLKDIV:  return 4000000u / period;            /* 40 MSPS / (ratio/100) */
    default:               return 0u;
    }
}

const uint32_t *capture_sweep_periods(uint32_t *count)
{
    switch (pacing_cur) {
    case ADC_TRG2_REPEAT:  *count = sizeof sweep_repeat / sizeof sweep_repeat[0]; return sweep_repeat;
    case ADC_TRG2_SCCP1:   *count = sizeof sweep_sccp / sizeof sweep_sccp[0];     return sweep_sccp;
    case ADC_PACE_SINGLE:  *count = sizeof sweep_sccp / sizeof sweep_sccp[0];     return sweep_sccp;
    case ADC_PACE_CLKDIV:  *count = sizeof sweep_clkdiv / sizeof sweep_clkdiv[0]; return sweep_clkdiv;
    default:               *count = 1u;                                          return sweep_b2b;
    }
}

const uint32_t *capture_sweep_periods2(uint32_t *count)
{
    if (pacing_cur == ADC_PACE_CLKDIV) {
        *count = sizeof sweep_clkdiv2 / sizeof sweep_clkdiv2[0];
        return sweep_clkdiv2;
    }
    *count = 0u;
    return NULL;
}

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
    const bool     was_running = capture_settle();   /* defined start   */
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
 * Rate self-test and the choice of pacing
 *
 * Timer1 (timebase.c) is only the stopwatch; the pacing source is what
 * produces the rate. For the active source: RATETEST_HALVES halves at
 * a slow period and at one four times shorter, ticks counted, delivered
 * rate compared with the nominal one (10 %) and the two with each other
 * (3..5x). The second check is the one that catches "the rate does not
 * move", which is what the board showed with back-to-back and SAMC.
 * The burst restart from the DMA interrupt costs a little per 2048
 * samples; the tolerance covers it.
 *
 * capture_autopace() runs this for every candidate when ADC_PACING is
 * AUTO and prints one verdict per source, so the log shows what each
 * one delivered - not only which one won.
 * ------------------------------------------------------------------ */
#define RATETEST_HALVES   200u
#define RATETEST_TOL_PCT  10u

#ifndef __MPLAB_DEBUGGER_SIMULATOR
/* Delivered rate at `period` (0 = leave the period alone, for B2B). */
static uint32_t rate_measure(uint32_t period, uint32_t *ksps)
{
    (void)capture_settle();                 /* defined start            */
    if (period != 0u) { (void)capture_set_period(period); }  /* idle: now */
    counters_clear();
    const uint32_t target = blocks_done + RATETEST_HALVES;
    const uint32_t t0     = timebase_ticks();
    capture_start();
    const uint32_t rc     = wait_for_blocks(target);
    const uint32_t ticks  = timebase_ticks() - t0;
    (void)capture_settle();                 /* test over: DMA down      */
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
    const uint8_t  pacing      = pacing_cur;
    const uint32_t keep        = capture_period();
    const bool     was_running = capture_running();
    uint32_t       ksps[2]     = { 0u, 0u };
    uint32_t       rc          = 0u;
    uint32_t       periods[2]  = { 0u, 0u };

    console_puts("[ratetest] pacing: ");
    console_puts(capture_pacing_name());
    console_puts("\r\n");

    switch (pacing) {
    case ADC_TRG2_REPEAT: periods[0] = 16u; periods[1] = 4u; break;  /* 5 / 20 MSPS */
    case ADC_TRG2_SCCP1:  periods[0] = 20u; periods[1] = 5u; break;  /* 5 / 20 MSPS */
    case ADC_PACE_CLKDIV: periods[0] = 800u; periods[1] = 200u; break; /* 5 / 20 MSPS */
    case ADC_PACE_SINGLE: periods[0] = 20u; periods[1] = 5u; break;  /* 5 / 20 MSPS */
    default:
        /* Back-to-back: nothing to judge against, just say what it delivers. */
        rc = rate_measure(0u, &ksps[0]);
        if (rc == 0u) {
            console_kv("[ratetest]   measured ksps (no period to compare with)", ksps[0]);
        } else {
            console_puts("[ratetest]   no data\r\n");
        }
        goto restore;
    }

    for (uint32_t i = 0; (i < 2u) && (rc == 0u); i++) {
        rc = rate_measure(periods[i], &ksps[i]);
        if (rc == 0u) {
            const uint32_t nominal = capture_nominal_ksps(periods[i]);
            const uint32_t diff    = (ksps[i] > nominal) ? ksps[i] - nominal : nominal - ksps[i];
            console_kv("[ratetest]   period", periods[i]);
            console_kv("[ratetest]     nominal ksps", nominal);
            console_kv("[ratetest]     measured ksps", ksps[i]);
            if (diff > nominal * RATETEST_TOL_PCT / 100u) {
                console_puts("[ratetest]     outside the 10 % window\r\n");
                rc = 12u;
            }
        } else {
            console_kv("[ratetest]   no data at period", periods[i]);
        }
    }
    if (rc == 0u) {
        /* The short period must deliver four times the rate (3..5x). */
        if ((ksps[1] < 3u * ksps[0]) || (ksps[1] > 5u * ksps[0])) {
            console_puts("[ratetest]   the rate does not follow the period\r\n");
            rc = 12u;
        }
    }

restore:
    if (keep != 0u) { (void)capture_set_period(keep); }
    counters_clear();
    if (was_running) { capture_start(); }
    console_puts((rc == 0u) ? "[ratetest]   PASS\r\n" : "[ratetest]   FAIL\r\n");
    return rc;
#endif
}

uint32_t capture_autopace(void)
{
    console_kv("[pacing] time base check, ticks per 100 ms (expect 1250000)", timebase_check());
#if ADC_PACING != 0u
    (void)capture_set_pacing((uint8_t)ADC_PACING);
    console_puts("[pacing] fixed by ADC_PACING: ");
    console_puts(capture_pacing_name());
    console_puts("\r\n");
    return capture_ratetest();
#else
    /* Order = preference: the first paced source that passes is used.
     * Microchip's own mechanism first (one conversion per SCCP1 trigger),
     * then the clock divider, then the two burst triggers the board did
     * not follow in runs 5 and 6, back-to-back last as the reference. */
    static const uint8_t candidates[5] = { ADC_PACE_SINGLE, ADC_PACE_CLKDIV, ADC_TRG2_REPEAT, ADC_TRG2_SCCP1, ADC_TRG2_B2B };
    uint32_t result[5];
    uint8_t  chosen = ADC_TRG2_B2B;
    bool     have   = false;

    for (uint32_t i = 0; i < 5u; i++) {
        (void)capture_set_pacing(candidates[i]);
        result[i] = capture_ratetest();
        if ((result[i] == 0u) && !have && (candidates[i] != ADC_TRG2_B2B)) {
            chosen = candidates[i];
            have   = true;
        }
    }
    (void)capture_set_pacing(chosen);

    console_puts("[pacing] summary: single per SCCP1 trigger ");
    console_puts((result[0] == 0u) ? "PASS" : "FAIL");
    console_puts(", ADC clock divider ");
    console_puts((result[1] == 0u) ? "PASS" : "FAIL");
    console_puts(", repeat timer ");
    console_puts((result[2] == 0u) ? "PASS" : "FAIL");
    console_puts(", SCCP1 as burst trigger ");
    console_puts((result[3] == 0u) ? "PASS" : "FAIL");
    console_puts(", back-to-back ");
    console_puts((result[4] == 0u) ? "runs" : "no data");
    console_puts("\r\n[pacing] using: ");
    console_puts(capture_pacing_name());
    console_puts(have ? "\r\n"
                      : " - NO PACED SOURCE PASSED, the rate is not under control\r\n");
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
    console_kv_hex("buf_end", (uint32_t)&buf[SAMPLES_PER_BUF]);
    console_kv_hex("guard0", dma_buffer.guard[0]);
    console_kv_hex("guard1", dma_buffer.guard[1]);
    console_kv_hex("guard15", dma_buffer.guard[BUF_GUARD_WORDS - 1u]);
}

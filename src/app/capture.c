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
#include "sccp.h"
#include "stats.h"
#include "pingpong.h"
#include "capture_priv.h"
#include "sigproc.h"

/* SELFTEST_* moved to meter.c with capture_selftest() (P9.3, 27.09.2026). */

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
    uint16_t data[SAMPLES_PER_ALLOC];       /* pairs A and B (capture.h) */
    uint32_t guard[BUF_GUARD_WORDS];
} dma_buffer __attribute__((section(".dma_buffer"), aligned(4)));
#define buf (dma_buffer.data)

/* pingpong.c's own state (P9.1): proc_missed's computation and the
 * main-loop service bookmark - see pingpong.h for why late_service/
 * dma_overrun stay capture.c's own globals instead. The buffer, the
 * guard words and blocks_done/ready_half/last_sample below are NOT
 * cached here either - every call passes them in fresh (pingpong.h: it
 * is what keeps dma0_event() cheap). Zero-initialised by C's rule for
 * static storage, exactly the state pingpong_counters_clear() would
 * otherwise have to produce. */
static pingpong_t pp;

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
volatile uint32_t ready_half    = 0;   /* 0 = first half, 1 = second half  */
volatile uint32_t selftest_mean = 0;   /* mean seen on ADxAN6, ~3840       */
volatile int32_t  proc_result   = 0;   /* output of process_buffer()       */

/* Three counters that exist for one question, raised by run 14: the
 * sweep reported overruns at 4 MSPS, where the DMA has eight times the
 * headroom it needs, and its loaded rate column read ten times the rate
 * a clean burst measures at the same setting. Two explanations fit and
 * they need separating.
 *
 *   (a) one conversion produces several DMA transfers. Microchip
 *       acknowledges exactly that for this silicon: "ADC triggers for DMA
 *       on this device have an issue. A few transfers are possible per
 *       one trigger."
 *   (b) the handler books the same HALF or DONE more than once, because
 *       the status flag did not clear and every later entry - and at full
 *       rate there are 1.6 million of them a second, one per overrun -
 *       sees it again. That would be our bug, and fixable.
 *
 * The three numbers tell them apart. If half_events is of the order of
 * the overrun count, the flags are not clearing: (b). If they stay at one
 * per 1024 transfers while the counts still race, the transfers really
 * are happening: (a). */
volatile uint32_t isr_entries   = 0;   /* calls of dma0_event()            */
volatile uint32_t half_events   = 0;   /* HALF seen set                    */
volatile uint32_t done_events   = 0;   /* DONE seen set                    */
/* The one that decides it. One burst is CNT = 2 * half_len conversions,
 * which is exactly one buffer: HALF in the middle, DONE at the end. So in
 * back-to-back streaming blocks_done MUST be exactly twice the number of
 * bursts started - that is arithmetic, not an assumption about the
 * silicon. If it is ten times that, the handler is booking the same event
 * over and over and the fault is ours. If it holds while the rate still
 * races, the transfers really are happening and it is the trigger defect
 * Microchip acknowledges. Note that half_events + done_events == blocks
 * by construction and therefore proves nothing on its own. */
volatile uint32_t burst_starts  = 0;   /* calls of start_burst()           */

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

/* Ping-pong pairs (01.10.2026). The triggered stream runs dma.c's pair
 * mode: channel 0 fills the ping half, channel 1 the pong half, of pair
 * A (buf[0..2H-1]) or pair B (buf[2H..4H-1]), H = half_len. For a
 * "stream grab", capture_pair_freeze() asks the DMA event below to move
 * on to the other pair: at the ping-complete event the waiting channel 0
 * gets the other pair's ping half, at the pong-complete event the then
 * waiting channel 1 its pong half - so the pair just completed, ping and
 * pong in order, stands still and the stream never stops (the waiting
 * channel's address can be moved at any time, a running one's cannot -
 * dma.h). The back-to-back burst mode keeps the single channel on pair A.
 *   pair_base   first sample of the pair being filled
 *   ready_off   first sample of the half completed last - one word, so
 *               the main loop never sees a half number from one event
 *               with a pair from another
 *   pair_req    a switch asked for; pair_moving: channel 0 already moved
 *   pair_frozen first sample of the pair that stands still, or PAIR_NONE */
#define PAIR_NONE  0xFFFFFFFFu
static volatile uint32_t pair_base   = 0u;
static volatile uint32_t ready_off   = 0u;
static volatile bool     pair_mode   = false;
static volatile bool     pair_req    = false;
static volatile bool     pair_moving = false;
static volatile uint32_t pair_frozen = PAIR_NONE;

/* For sigproc_info_t.gap: the block number last handed to sigproc_block(),
 * and "nothing handed over since the processing was switched on or the
 * counters cleared" (every stream start clears them). */
static uint32_t      sp_last_seq  = 0u;
static volatile bool sp_fresh     = true;

/* Channel reconfiguration requested by the console or the self-test,
 * applied by the ISR between two bursts, when the channel is idle. */
static volatile bool    switch_pending = false;
static volatile uint8_t pinsel_next    = ADC_PINSEL;
static volatile uint8_t samc_next      = ADC_SAMC;
/* The ADC clock divide ratio in hundredths, as last set successfully.
 * The hardware's own answer is clock_adc_div(); this is what was asked
 * for, so that a switch that did not arrive can be told from one that
 * did (capture_clkdiv_wanted vs capture_clkdiv). Non-static since P9.4
 * (27.09.2026): acquisition.c's capture_set_clkdiv() writes it; reached
 * through capture_priv.h, the same "plain extern" pattern oneshot_left
 * already uses (P9.3). */
uint32_t                  clkdiv_cur     = ADC_CLKDIV;

/* Emergency brake against the overrun interrupt storm.
 *
 * At the undivided rate the DMA loses about 4 % of the samples, and
 * every lost sample raises the channel interrupt: 1.6 million per
 * second, one every 625 ns, which is 125 CPU cycles at 200 MHz. An
 * interrupt entry with its context save plus this handler costs about
 * as much, so the CPU sits exactly on the edge of never returning to
 * the main loop - and twice it fell off: run 7 stopped in the DAC test,
 * run 8 in the sweep, both silently, both with the console dead (U2RX
 * is priority 1, the DMA channel 4). There is no enable bit for the
 * overrun event on its own (13.6.1; DMA0CH has HALFEN, DONEEN and
 * MATCHEN only), so it cannot be masked away.
 *
 * The handler is the only code still running when that happens, so the
 * handler is what stops it: past OVERRUN_LIMIT in one measurement it
 * masks its own interrupt, takes the channel down and ends the stream.
 * The rate is then reported as unusable instead of the board going
 * quiet. The limit is well above what a healthy full-rate point
 * produces (about 82 000 per 2000 halves, run 8) and is reached within
 * a third of a second once the storm runs away. counters_clear(), which
 * every test calls first, re-arms it. */
#define OVERRUN_LIMIT     500000u
static volatile bool     overrun_abort  = false;
/* The brake counts for itself, from zero at every capture_start().
 * It must not use dma_overrun: that one is the measurement and only
 * counters_clear() resets it, so after the brake had fired once the
 * very first overrun of every later test tripped it again and every
 * test reported "DMA channel disabled" (run 9). */
static volatile uint32_t overrun_run    = 0;
/* One burst and then stop, decided in the ISR (capture_oneshot, now
 * meter.c). Non-static since P9.3 (27.09.2026): capture_oneshot_n() in
 * meter.c sets/clears them, dma0_event() below still reads/decrements
 * oneshot_left directly - see capture_priv.h. */
volatile uint32_t oneshot_left   = 0u;  /* bursts still to run */
volatile uint32_t oneshot_ticks  = 0;   /* of the burst alone */

/* The triggered stream of the chain test (capture_chain_start): SCCP1
 * paces the ADC in Single Conversion mode, the DMA runs on by itself, and
 * DONE restarts nothing - there is no burst. chain_stop_after counts DONE
 * events down to an automatic stop from the ISR (0 = run until told).
 * chain_t0/t1 are Timer1 at the SCCP1 start and stop, the window the
 * expected number of triggers is computed from. */
static volatile bool     chain_mode       = false;
static volatile uint32_t chain_stop_after = 0u;
static volatile uint32_t chain_t0         = 0u;
static volatile uint32_t chain_t1         = 0u;
static bool              chain_src_data   = false;  /* DMA from CH0DATA  */
/* The trigger period and mode of the running chain, kept so that
 * capture_chain_resume() can restart the SAME trigger after
 * capture_chain_halt() paused it - a halt/resume pair never re-derives
 * the rate, it only stops and starts SCCP1. */
static uint32_t          chain_ticks      = 0u;
static uint32_t          chain_sccp_mode  = 0u;

/* What the processing of a half costs, in Timer1 ticks (80 ns): the
 * budget question of the example's sentence ("the CPU processes"). */
volatile uint32_t proc_ticks_max = 0;
volatile uint32_t proc_ticks_sum = 0;
volatile uint32_t proc_count     = 0;

/* capture.h: the defined idle state every test starts from. */
bool capture_settle(void)
{
    const bool was_running = run_enabled;
    capture_stop();
    if (chain_mode) {
        /* Triggered stream: the trigger goes first (ANALYSIS.md C.10
         * point 4), then nothing converts any more and nothing is in
         * flight to wait for. */
        sccp1_stop();
        if (chain_t1 == 0u) { chain_t1 = timebase_ticks(); }
        burst_active = false;
        chain_mode   = false;
    }
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
    pair_req    = false;              /* no pair move left pending      */
    pair_moving = false;
    ready_half = 0u;
    return was_running;
}
static bool quiesce(void) { return capture_settle(); }

/* A burst is in flight from here until the DMA DONE interrupt. */
static void start_burst(void)
{
    burst_starts++;
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
    const uint32_t used = 4u * half_len;      /* both pairs, A and B */
    if (used + 2u * BUF_GUARD_WORDS <= SAMPLES_PER_ALLOC) {
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
    pair_base   = 0u;
    ready_off   = 0u;
    pair_req    = false;
    pair_moving = false;
    pair_frozen = PAIR_NONE;
    pair_mode   = chain_mode;
    if (pair_mode) {
        /* the triggered stream: channels 0+1 in hardware ping-pong over
         * pairs A and B (dma.h) */
        dma0_pp_init(adc_dma_trigger(),
                     chain_src_data ? adc_dma_source_data() : adc_dma_source(),
                     dma_buffer.data, half_len * sizeof(uint16_t));
    } else {
        dma0_init(adc_dma_trigger(),
                  chain_src_data ? adc_dma_source_data() : adc_dma_source(),
                  dma_buffer.data, 2u * half_len * sizeof(uint16_t));
    }
    dma_armed = true;
}
_Static_assert(sizeof dma_buffer.data == SAMPLES_PER_ALLOC * sizeof(uint16_t),
               "buffer allocation and its maximum must be the same thing");

uint32_t capture_half_len(void) { return half_len; }

bool capture_set_half_len(uint32_t n)
{
    if ((n < SAMPLES_PER_HALF_MIN) || (n > SAMPLES_PER_HALF_MAX)) { return false; }
    /* Even only: process_buffer() reads a half as 32-bit words, and with
     * an odd length the second half would start on an odd sample - an
     * unaligned word access, which traps. */
    if ((n & 1u) != 0u) { return false; }
    if (run_enabled || burst_active) { return false; }      /* stop first */
    (void)capture_settle();           /* DMA down; the next start re-inits */
    half_len = n;
    return true;
}

/* The same check without stopping, for a test that reports it. Delegated
 * to pingpong.c (P9.1); guard_word() still gives it the base pointer, in
 * capture_init(), because where the guard words are is capture.c's own
 * buffer layout decision. */
bool capture_guard_ok(void)
{
    return pingpong_guard_ok(guard_word(0), BUF_GUARD_WORDS, BUF_GUARD_PATTERN(0));
}

/* Stop with code 11 if anything wrote past the end of the buffer. The
 * check itself is pingpong_guard_ok(); the per-index report on the first
 * mismatch stays here, unchanged, since it prints. */
static void guard_check(void)
{
    if (pingpong_guard_ok(guard_word(0), BUF_GUARD_WORDS, BUF_GUARD_PATTERN(0))) {
        return;
    }
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
    isr_entries++;
    if (st & DMA0_OVERRUN) {
        /* Triggered while the previous transfer was still in progress
         * (p816): the bus did not keep up. This is the measurement. */
        dma_overrun++;
        overrun_run++;
        dma0_clear(DMA0_OVERRUN);
        if (overrun_run >= OVERRUN_LIMIT) {
            /* The brake (see OVERRUN_LIMIT above). Masking the interrupt
             * and disabling the channel here, from inside the storm, is
             * what gives the main loop the CPU back. */
            dma0_halt();              /* IEC2.DMA0IE = 0, CHEN = 0      */
            run_enabled   = false;
            burst_active  = false;
            dma_armed     = false;    /* the next start re-initialises   */
            overrun_abort = true;
            return;
        }
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
     * overwritten by the DMA reload. Kept as the same two raw tests of
     * `st` the code below tests again per branch - not a shared boolean -
     * so that each branch below stays the single, self-contained `if`
     * it was before this task; pingpong.h's design note says why that
     * matters here. */
    if ((st & DMA0_HALF) && (st & DMA0_DONE)) {
        late_service++;
    }

    if (st & DMA0_HALF) {
        half_events++;
        dma0_clear(DMA0_HALF);
        /* Which half is ready, the last sample, blocks_done - now
         * pingpong.c's pingpong_on_half() (P9.1), `static inline` so
         * this stays the same handful of instructions it always was. */
        pingpong_on_half(&buf[pair_base], half_len, &blocks_done, &ready_half,
                         &last_sample, false);
        ready_off = pair_base;
        /* Pair mode: channel 1 now writes the pong half, channel 0 waits -
         * the moment to send channel 0 to the other pair if a grab asked. */
        if (pair_req && !pair_moving) {
            const uint32_t other = (pair_base == 0u) ? 2u * half_len : 0u;
            dma0_pp_set_dst(0u, &buf[other]);
            pair_moving = true;
        }
    }
    if (st & DMA0_DONE) {
        done_events++;
        dma0_clear(DMA0_DONE);
        pingpong_on_half(&buf[pair_base], half_len, &blocks_done, &ready_half,
                         &last_sample, true);
        ready_off = pair_base + half_len;
        /* Pair mode: the pair is complete, ping and pong; channel 0 writes
         * the other pair's ping already, channel 1 waits and follows. The
         * completed pair stands still from here (capture_pair_freeze()). */
        if (pair_moving) {
            const uint32_t other = (pair_base == 0u) ? 2u * half_len : 0u;
            dma0_pp_set_dst(1u, &buf[other + half_len]);
            pair_frozen = pair_base;
            pair_base   = other;
            pair_moving = false;
            pair_req    = false;
        }

        /* The channel is idle between bursts: this is the only safe
         * moment to change its input or sample time. */
        if (switch_pending) {
            adc_set_input(pinsel_next, samc_next);
            switch_pending = false;
        }
        if (chain_mode) {
            /* Triggered stream: the DMA has reloaded by itself and the
             * next trigger continues at buf[0]. Nothing to restart - only
             * the optional stop after a number of blocks, taken here so
             * that the buffer holds exactly the last block, contiguous and
             * no longer written. The trigger goes off first. */
            if ((chain_stop_after != 0u) && (--chain_stop_after == 0u)) {
                sccp1_stop();
                chain_t1     = timebase_ticks();
                run_enabled  = false;
                burst_active = false;
            }
        } else if (oneshot_left != 0u) {
            /* One buffer and no more, decided here rather than by the
             * main loop: at the full rate the main loop can be tens of
             * milliseconds behind, and by the time it asked for a stop
             * the buffer would have been overwritten a thousand times
             * over. Stopping from the ISR leaves exactly the 2 * half_len
             * samples of this burst standing, contiguous and unmolested. */
            /* One burst less to go. Only the last one ends the run;
             * the others restart immediately, exactly as continuous
             * streaming does - which is the point of measuring more than
             * one (run 16). */
            oneshot_left--;
            if (oneshot_left != 0u) {
                start_burst();
            } else {
                run_enabled  = false;
                burst_active = false;
            }
        } else if (run_enabled) {
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
    overrun_run = 0u;                 /* the brake counts per run       */
    overrun_abort = false;
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
    adc_init(pinsel, samc, SAMPLES_PER_BUF_MAX); /* core on, ADRDY, or fail(5) */
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

/* The burst stream should run but its DMA channel is off - main()'s
 * fail(8) test. The three reads are taken in one go, with the console's
 * interrupts held off (DISICTL threshold 2: the UART receive interrupt,
 * where commands run, and the transmit one - DS70005591D 10.10.1.1,
 * p596); the DMA (4) and the counters (3) stay live. Until 01.10.2026
 * main() read capture_running(), capture_chain_active() and
 * dma0_enabled() one after the other, and a "stream off" landing between
 * the first read and the others produced a state that never existed -
 * running, no chain, DMA off - and a false fail 8. Reproduced on the
 * board: 1 in 9 "stream off" at 1 MSPS (docs/HARDWARE-LOG.md, 01.10.2026);
 * the "unexplained fail 8 after stream off" of 29.09.2026 (SG.3) was the
 * same race. The DMA interrupt's own changes (the overrun brake, the
 * last oneshot block) cannot fake it: each is one interrupt, and it
 * either lands before the reads or after them. */
bool capture_stream_lost(void)
{
#if defined(__XC_DSC__)   /* not under the trace harness's host gcc (diag.c) */
    const uint32_t old = DISIIPL;
    (void)__builtin_write_DISICTL((old > 2u) ? old : 2u);
#endif
    const bool lost = run_enabled && !chain_mode && !dma0_enabled();
#if defined(__XC_DSC__)
    (void)__builtin_write_DISICTL(old);
#endif
    return lost;
}

bool capture_overrun_aborted(void)
{
    return overrun_abort;
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
 * There is a gap between 20 and 40 MSPS and it is not an oversight: the
 * datasheet says FRACDIV does nothing while INTDIV is 0 (12.4.2 4b),
 * so no ratio between 1 and 2 can be realised at all. 20 MSPS is the
 * fastest divided rate; the next step up is the undivided clock.
 *
 * 500 = 8 MSPS is the rate the customer's application needs; it is in
 * the ladder for that reason and for no other.
 * ------------------------------------------------------------------ */
/* The rate ladder, slowest first. Each row is a pair of PLL1 output
 * dividers; the ADC clock is 1600 MHz / (POSTDIV1 * POSTDIV2) and eight
 * of those clocks make one back-to-back conversion.
 *
 * This replaced the CLKGEN6 divide ratios after run 9. Those were written,
 * read back and confirmed by DIVSWEN and CLKRDY at every single ratio, with
 * the generator switched off around the write and with it left running -
 * and the ADC converted at 40 MSPS throughout (docs/HARDWARE-LOG.md runs 8
 * and 9). The PLL's own output-divider switch is the one clock_init()
 * performs at boot, so it is known to work on this silicon.
 *
 * POSTDIV1 must not be smaller than POSTDIV2 (p778), and 7/7 = 32.65 MHz
 * is the slowest setting that still clears the ADC's 32 MHz minimum. */
static const struct pll_step sweep_steps[] = {
    { 7u, 7u },   /*  32.65 MHz   4.08 MSPS  slowest the ADC may run    */
    { 7u, 6u },   /*  38.10 MHz   4.76 MSPS                             */
    { 6u, 6u },   /*  44.44 MHz   5.56 MSPS                             */
    { 7u, 5u },   /*  45.71 MHz   5.71 MSPS                             */
    { 6u, 5u },   /*  53.33 MHz   6.67 MSPS                             */
    { 7u, 4u },   /*  57.14 MHz   7.14 MSPS                             */
    { 5u, 5u },   /*  64.00 MHz   8.00 MSPS  the customer's floor       */
    { 6u, 4u },   /*  66.67 MHz   8.33 MSPS                             */
    { 5u, 4u },   /*  80.00 MHz  10.00 MSPS                             */
    { 6u, 3u },   /*  88.89 MHz  11.11 MSPS                             */
    { 5u, 3u },   /* 106.67 MHz  13.33 MSPS                             */
    { 6u, 2u },   /* 133.33 MHz  16.67 MSPS                             */
    { 5u, 2u },   /* 160.00 MHz  20.00 MSPS                             */
    { 5u, 1u }    /* 320.00 MHz  40.00 MSPS  undivided, the boot setting */
};



/* capture_set_clkdiv()/_set_pll()/_set_rate() moved to acquisition.c
 * (P9.4, 27.09.2026); clkdiv_cur (above) is what capture_set_clkdiv()
 * writes, reached through capture_priv.h. */

uint32_t capture_clkdiv(void)        { return clock_adc_div(); }  /* hardware */
uint32_t capture_clkdiv_wanted(void) { return clkdiv_cur; }       /* asked for */

uint32_t capture_nominal_ksps(uint32_t ratio_h)
{
    (void)ratio_h;
    /* Eight ADC clocks per back-to-back conversion, and the clock is read
     * from the registers - so this is the rate the hardware is actually
     * set up for, not the one that was asked for. */
    return clock_adc_hz() / 8000u;
}

const struct pll_step *capture_sweep_steps(uint32_t *count)
{
    *count = sizeof sweep_steps / sizeof sweep_steps[0];
    return sweep_steps;
}


const volatile uint16_t *capture_completed_half(void)
{
    return &buf[ready_off];
}

/* "stream grab" (01.10.2026): move the stream on to the other pair and
 * hand back the pair just completed - ping then pong, contiguous in time
 * and in memory, still from here on. The stream does not stop: the DMA
 * event does the move at the next ping-complete/pong-complete events
 * (above). Meanwhile this keeps the main loop's work going, since it
 * runs inside a console command - capture_service() each turn, so no
 * half goes unprocessed while it waits (at 1 kSPS the move takes up to
 * three halves, 3 s). False if no triggered stream runs or the move did
 * not come within `max_ticks` (timebase). Release it with
 * capture_pair_release() once sent. */
bool capture_pair_freeze(uint32_t max_ticks, const volatile uint16_t **win,
                         uint32_t *n, uint32_t *from)
{
    if (!pair_mode || !chain_mode || !run_enabled) { return false; }
    pair_frozen = PAIR_NONE;
    pair_req    = true;
    const uint32_t t0 = timebase_ticks();
    while (pair_frozen == PAIR_NONE) {
        (void)capture_service();
        if ((timebase_ticks() - t0) > max_ticks) {
            pair_req = false;            /* the event may still move it: */
            return false;                /* harmless, the next grab waits */
        }
    }
    (void)capture_service();             /* the pong just completed      */
    *from = pair_frozen;
    *win  = &buf[pair_frozen];
    *n    = 2u * half_len;
    return true;
}

void capture_pair_release(void)
{
    pair_frozen = PAIR_NONE;
}

void counters_clear(void)
{
    sp_fresh      = true;             /* sigproc: the next block is a gap */
    overrun_abort = false;            /* re-arm the brake               */
    overrun_run   = 0;
    isr_entries = 0; half_events = 0; done_events = 0; burst_starts = 0;
    proc_ticks_max = 0; proc_ticks_sum = 0; proc_count = 0;
    dma_overrun = 0; dma_addr_err = 0; dma_bus_err = 0;
    late_service = 0;
    /* proc_missed and pingpong's own service bookmark: pingpong.c's
     * pingpong_counters_clear() (P9.1). Halves completed up to now are not
     * "missed" from here on - without that, the first capture_service()
     * after the self-test books its 12 unserviced halves as proc_missed,
     * and the heartbeat goes to the error rate before the measurement has
     * even started. */
    pingpong_counters_clear(&pp, blocks_done);
    proc_missed = pp.missed;
}

/* ------------------------------------------------------------------ *
 * Process one completed buffer half
 *
 * Since 01.10.2026 the stream's own processing is sigproc_block()
 * (sigproc.c), called from capture_service(); this function is now only
 * the back-to-back bench's fixed reference load (meter.c's
 * capture_process_bench()). It was the placeholder for the customer's
 * "+ and -" arithmetic, written as a plain accumulate so the cost of touching every sample is visible in
 * the measurement: at 40 MSPS this loop sees 40 million values per
 * second and per channel, and whether the CPU keeps up is as much a
 * question as the DMA bandwidth.
 * ------------------------------------------------------------------ */
/* Run 19 (25.09.2026) measured the first version of this loop - one
 * volatile 16-bit read and one add per sample - at about 23 CPU cycles
 * per sample, 118 us per half at every rate, which used up 92 % of the
 * half period at 8 MSPS. The half being processed is complete and the DMA
 * is writing the OTHER one, so it is read as plain memory here: two
 * samples per 32-bit load (the buffer is 4-byte aligned and a half is an
 * even number of samples), four loads per pass. The compiler barrier
 * keeps the reads after whatever told the caller the half was ready.
 * Same result as before: the sum of the half's samples. */
/* Non-static since P9.3 (27.09.2026): meter.c's capture_process_bench()
 * calls it too - see capture_priv.h for why it stays defined here
 * (capture_service() below is its other caller). */
void process_buffer(const volatile uint16_t *b, uint32_t n)
{
    __asm__ volatile ("" ::: "memory");
    const uint32_t *w = (const uint32_t *)(const volatile void *)b;
    const uint32_t words = n / 2u;
    uint32_t lo = 0u, hi = 0u;
    uint32_t i = 0u;
    for (; (i + 4u) <= words; i += 4u) {
        const uint32_t a = w[i], c = w[i + 1u], d = w[i + 2u], e = w[i + 3u];
        lo += (a & 0xFFFFu) + (c & 0xFFFFu) + (d & 0xFFFFu) + (e & 0xFFFFu);
        hi += (a >> 16) + (c >> 16) + (d >> 16) + (e >> 16);
    }
    for (; i < words; i++) {
        lo += w[i] & 0xFFFFu;
        hi += w[i] >> 16;
    }
    proc_result = (int32_t)(lo + hi);
    SIM_CHECK_HALF(b, n);             /* simulator: is this really the next half? */
}

/* capture_process_bench() moved to meter.c with the other back-to-back
 * instruments (P9.3, 27.09.2026); half_mean() (lib/stats.c, P2.2) moved
 * with it - capture_selftest() is its only caller. */

/* The signal processing switch ("sigproc on|off", off after reset) and
 * the flag that says capture_service() is running. While that flag is
 * set, cli.c's uart_rx_hook() holds received bytes back instead of
 * running a command (capture_sigproc_busy()), so no console command can
 * interrupt capture_service() part-way through:
 *   - with the processing on, "stream grab" would otherwise send a
 *     half-processed block (sigproc.h); set for the WHOLE call, not only
 *     around sigproc_block(), since a grab between pingpong_service()
 *     and the processing would find the half seen but not processed;
 *   - and since 01.10.2026 always, processing on or off: "stream grab"
 *     itself calls capture_service() while it waits for the pair move
 *     and while it sends, and a command landing inside the main loop's
 *     own call would run a second capture_service() over the first -
 *     two writers of pingpong's bookkeeping at once, which the board
 *     showed as a negative "missed" (-2) in a grab. Without processing a
 *     call takes microseconds, so the delay for a command is nothing. */
static volatile bool sigproc_on   = false;
static volatile bool sigproc_busy = false;

void capture_sigproc_enable(bool on) { if (on && !sigproc_on) { sp_fresh = true; } sigproc_on = on; }
bool capture_sigproc_enabled(void)   { return sigproc_on; }
bool capture_sigproc_busy(void)      { return sigproc_busy; }

static bool service_once(bool processing);

bool capture_service(void)
{
    const bool processing = sigproc_on;     /* one reading for this half */
    sigproc_busy = true;
    const bool served = service_once(processing);
    sigproc_busy = false;
    return served;
}


static bool service_once(bool processing)
{
    if (!pingpong_service(&pp, blocks_done)) {
        return false;
    }
    proc_missed = pp.missed;
    /* The completed half goes to the signal processing (sigproc.c), ping
     * and pong alike, when it is switched on. ready_off and half_len are
     * read once, so the pointer, the length and info.half agree even if
     * the next DMA event lands in between. The DMA is writing the other
     * half, so the block is handed over as plain memory, to read and to
     * write the result back into; the compiler barrier keeps every access
     * after the ready check above (process_buffer() did the same until
     * 01.10.2026 - it stays for meter.c's bench). The simulator's
     * ping-pong order check moved here from process_buffer(): it judges
     * the service, not the processing, and must run with it off too. The
     * time is measured either way - with the processing off it is the
     * cost of the service alone. */
    const uint32_t off = ready_off;
    const uint32_t n = half_len;
    const uint32_t h = ((off / n) & 1u);  /* ping or pong, from the same read */
    const volatile uint16_t *half = &buf[off];
    SIM_CHECK_HALF(half, n);          /* simulator: is this really the next half? */
    const uint32_t seq = pp.seen_blocks;
    const sigproc_info_t info = { h, seq, proc_missed,
                                  (sp_fresh || (seq != sp_last_seq + 1u)) ? 1u : 0u };
    __asm__ volatile ("" ::: "memory");
    const uint32_t t0 = timebase_ticks();
    if (processing) {
        sigproc_block((uint16_t *)(volatile void *)half, n, &info);
        sp_last_seq = seq;
        sp_fresh    = false;
    }
    __asm__ volatile ("" ::: "memory");
    const uint32_t dt = timebase_ticks() - t0;
    if (dt > proc_ticks_max) { proc_ticks_max = dt; }
    proc_ticks_sum += dt;
    proc_count++;
    guard_check();                    /* did the DMA stay inside buf?    */

    /* Heartbeat: slow while clean, fast once any error counter moved.
     * pp.seen_blocks is the blocks_done snapshot pingpong_service() just
     * took - the same single, consistent reading capture_service() used
     * to keep in its own local `done` for this. */
    if (led_get_mode() == 2u) {
        const bool clean = (dma_overrun | dma_addr_err | dma_bus_err |
                            late_service | proc_missed) == 0u;
        if ((pp.seen_blocks % (clean ? HEARTBEAT_OK : HEARTBEAT_ERR)) == 0u) {
            led_toggle();
        }
    }
    return true;
}

/* Wait until blocks_done passes a value. Returns 0, or 6 (nothing
 * moves) or 8 (the DMA switched itself off, e.g. on an address fault).
 * Non-static since P9.3 (27.09.2026): meter.c's capture_selftest()/
 * _clkoff_probe()/_oneshot_n()/_measure_rate() call it too - see
 * capture_priv.h for why it stays defined here (it calls guard_check(),
 * which reads the private DMA buffer's guard words directly, and
 * capture_service() below calls it too). */
uint32_t wait_for_blocks(uint32_t target)
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

/* capture_selftest() and capture_clkoff_probe() (with the "Delivered
 * rate" comment that used to introduce it here) moved to meter.c with the
 * other back-to-back instruments (P9.3, 27.09.2026). */

/* The variant matrix (capture_variant_t, capture_select_variant() and its
 * reporting functions) moved to acquisition.c with the rate setters above
 * (P9.4, 27.09.2026). */

/* ------------------------------------------------------------------ *
 * The triggered stream - the chain the example is about (capture.h)
 *
 *   SCCP1 (CLKGEN13) -> ADC channel 0, Single Conversion, TRG1SRC 0x20
 *     -> "ADCn Done CH0" -> DMA0, Repeated One-Shot -> buf -> HALF/DONE
 *
 * Start order DMA, ADC, trigger last; stop order trigger first
 * (ANALYSIS.md C.10 point 4). The ADC is expected in Single mode already
 * (adc_set_mode_single(), done by the chain test once per core switch).
 * ------------------------------------------------------------------ */
bool capture_chain_start(uint32_t ticks, uint32_t sccp_mode,
                         uint32_t stop_after_done, bool src_data)
{
    (void)capture_settle();           /* idle, DMA down                  */
    counters_clear();
    chain_src_data  = src_data;
    chain_ticks     = ticks;
    chain_sccp_mode = sccp_mode;
    chain_mode      = true;           /* before capture_init(): pair mode */
    capture_init();                   /* DMA armed on RES or DATA        */
    adc_clear_events();
    chain_stop_after = stop_after_done;
    chain_t1     = 0u;
    chain_mode   = true;
    run_enabled  = true;
    burst_active = true;              /* "something runs" for the waits  */
    chain_t0 = timebase_ticks();
    if (!sccp1_start(ticks, SCCP_CLK_GEN13, (sccp_mode_t)sccp_mode, SCCP_EVENT_SPECIAL)) {
        (void)capture_settle();
        chain_src_data = false;
        return false;
    }
    return true;
}

bool capture_chain_active(void) { return chain_mode && run_enabled; }

uint32_t capture_chain_wait(uint32_t max_ticks)
{
    const uint32_t t0 = timebase_ticks();
    while (chain_mode && run_enabled) {
        if (!dma0_enabled())                     { return 8u; }
        if ((timebase_ticks() - t0) > max_ticks) { return 6u; }
    }
    return 0u;
}

uint64_t capture_transfers(void)
{
    /* Whole blocks plus the position in the current one. DMA0CNT counts
     * down and is reloaded at DONE, so right after a DONE the position is
     * 0 - and done_events has already been booked by the ISR, which runs
     * at priority 4, above every caller of this. */
    const uint32_t block = 2u * half_len;
    const uint32_t left  = dma0_remaining();
    const uint32_t pos   = (left <= block) ? (block - left) : 0u;
    return (uint64_t)done_events * block + pos;
}

uint64_t capture_chain_stop(void)
{
    sccp1_stop();                     /* trigger first                   */
    if (chain_t1 == 0u) { chain_t1 = timebase_ticks(); }
    /* The last conversion and its transfer: sample time plus conversion
     * plus one DMA transaction, well under a microsecond. Wait 2 us. */
    const uint32_t t = timebase_ticks();
    while ((timebase_ticks() - t) < 25u) { }
    const uint64_t n = capture_transfers();
    (void)capture_settle();
    chain_src_data = false;
    return n;
}

uint32_t capture_chain_window_ticks(void)
{
    return chain_t1 - chain_t0;
}

/* capture.h: halt / resume an already-running chain stream in place, for
 * the GUI's halt/grab/restart cycle (chain_stream_grab_begin/_end,
 * chaintest.c). Unlike capture_chain_stop(), the DMA channel is left
 * armed and counters_clear() is not called - a grab is meant to be
 * invisible to everything except the trigger. */
bool capture_chain_halt(void)
{
    if (!chain_mode || !run_enabled) { return false; }
    sccp1_stop();                     /* trigger first (C.10 point 4)    */
    /* The one conversion already in flight and its DMA transaction, the
     * same bound capture_chain_stop() waits on. */
    const uint32_t t = timebase_ticks();
    while ((timebase_ticks() - t) < 25u) { }
    run_enabled  = false;
    burst_active = false;
    return true;
}

bool capture_chain_resume(void)
{
    if (!chain_mode || run_enabled) { return false; }
    burst_active = true;
    const bool ok = sccp1_start(chain_ticks, SCCP_CLK_GEN13,
                                (sccp_mode_t)chain_sccp_mode, SCCP_EVENT_SPECIAL);
    run_enabled = ok;
    if (!ok) { burst_active = false; }
    return ok;
}

void capture_fill(uint16_t v)
{
    for (uint32_t i = 0; i < SAMPLES_PER_ALLOC; i++) { buf[i] = v; }
}

/* capture_oneshot() and capture_oneshot_n() moved to meter.c with the
 * other back-to-back instruments (P9.3, 27.09.2026); oneshot_left/
 * oneshot_ticks stay defined here (capture_priv.h) because dma0_event()
 * below reads oneshot_left on every DONE. */

uint32_t capture_oneshot_ticks(void)
{
    return oneshot_ticks;
}

const volatile uint16_t *capture_buffer(void)
{
    return &buf[0];
}

/* capture_measure_rate() moved to meter.c with the other back-to-back
 * instruments (P9.3, 27.09.2026). */

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

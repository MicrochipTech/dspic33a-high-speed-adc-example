/*
 * sim_dma.c
 *
 * Stand-in for dma.c in the MPLAB X simulator build. Implements the
 * dma.h interface without a DMA: dma0_init() only remembers where the
 * data is to go, and sim_dma_tick() (sim.h) fills the next buffer half
 * the way the DMA would and then hands dma0_event() the same status
 * word the interrupt would have taken from DMA0STAT. Everything
 * downstream - blocks_done, ready_half, last_sample, the input switch at
 * DONE, the burst restart, capture_service(), the self-test, the status
 * lines - runs unchanged, in capture.c.
 *
 * Why: the simulator has neither the ADC nor the DMA, and it does not
 * dispatch interrupts in this project either (tools/sim_trap.py, the
 * E0110 note in docs/TROUBLESHOOTING.md). So the only producer of
 * measurement data on silicon, the DMA interrupt, never runs there.
 *
 * Signal: on the self-test input (ADxAN6, PINSEL 6) a flat 3840, which
 * is 15/16 * 4096 and what the self-test expects; on any other input a
 * 1 MHz sine, 2048 +/- 1600 counts. 1 MHz at 40 MSPS is 40 samples per
 * period, which is what this ADC is for: an 8 MSPS converter would see
 * the same tone with 8 points. A half of 1024 samples holds 25.6
 * periods, so the phase runs on across the half boundary like a real
 * signal would, and the per-half sum (proc_result) alternates between
 * two values - a cheap check that the halves are really served in order.
 *
 * Timing is not modelled: a half arrives per call, whenever the loop
 * calls. It is called from wait_for_blocks() and from main()'s loop
 * (SIM_DMA_TICK), so exactly one half is produced per service and
 * proc_missed stays 0.
 *
 * Fault injection from MDB, while halted or running:
 *   write <addr of sim_fault_once> <value>
 * DMA0STAT bit masks (0x3FF) go into the next status word, so the
 * OVERRUN / ADRERR / BWERR paths and the counters can be exercised;
 * SIM_FAULT_DROP_SAMPLE (65536) instead drops one sample of the sine
 * before the next half - a data fault the ping-pong check must catch at
 * index 0 of that half. Keep values below 2^31: MDB takes a larger
 * decimal as a 64-bit value and writes two words, which clobbers the
 * variable behind this one. The address comes from xc-dsc-nm on the
 * ELF; tools/sim_trap.py --fault does the lookup.
 */

#include <xc.h>
#include <stddef.h>
#include "dma.h"
#include "sim.h"
#include "adc.h"
#include "capture.h"
#include "console.h"

#define SIM_FLAT_PINSEL   6u              /* ADxAN6: the self-test input   */
#define SIM_SINE_PERIOD   40u             /* samples: 40 MSPS / 40 = 1 MHz */

static const uint16_t sim_sine[SIM_SINE_PERIOD] = {   /* 2048 + 1600 sin */
    2048, 2298, 2542, 2774, 2988, 3179, 3342, 3474, 3570, 3628,
    3648, 3628, 3570, 3474, 3342, 3179, 2988, 2774, 2542, 2298,
    2048, 1798, 1554, 1322, 1108,  917,  754,  622,  526,  468,
     448,  468,  526,  622,  754,  917, 1108, 1322, 1554, 1798
};

#define SIM_FAULT_DROP_SAMPLE  0x00010000u
volatile uint32_t sim_fault_once = 0;

/* What dma0_init() was told. The stand-in honours the channel's
 * parameters instead of reaching for capture.c's buffer, so that it
 * stays a drop-in for dma.c. */
static volatile uint16_t *sim_dst      = NULL;
static uint32_t           sim_half_len = 0;     /* count / 2              */
static bool               sim_enabled  = false; /* CHEN of the real one   */

/* ------------------------------------------------------------------ *
 * dma.h, simulated
 * ------------------------------------------------------------------ */
void dma0_init(uint32_t trigger, const volatile void *src,
               volatile void *dst, uint32_t count)
{
    (void)trigger;                        /* no trigger: the loop ticks  */
    (void)src;                            /* no source: a table          */
    sim_dst      = (volatile uint16_t *)dst;
    sim_half_len = count / 2u;
    sim_enabled  = true;
    console_puts("[dma] simulator stand-in armed: halves come from sim_dma_tick()\r\n");
}

bool dma0_enabled(void)
{
    return sim_enabled;
}

void dma0_halt(void)
{
    sim_enabled = false;
}

void dma0_clear(uint32_t flags)
{
    (void)flags;                          /* no status register here     */
}

void dma0_regs_dump(void)
{
    console_puts("[regs] dma (simulator stand-in, no registers)\r\n");
    console_kv("sim_enabled", sim_enabled ? 1u : 0u);
    console_kv_hex("sim_dst", (uint32_t)sim_dst);
    console_kv("sim_half_len", sim_half_len);
}

/* ------------------------------------------------------------------ *
 * The producer: one half per call
 * ------------------------------------------------------------------ */
void sim_dma_tick(void)
{
    static uint32_t half  = 0u;
    static uint32_t phase = 0u;

    if (!sim_enabled || !capture_burst_active()) {
        return;                           /* stopped: no DMA events either */
    }

    /* Take the injected fault once, at the top. MDB writes the variable
     * at any moment; if it were read here and cleared after the fill
     * loop below, a write landing during the loop would be wiped without
     * ever being seen (that happened: the negative test passed). */
    const uint32_t fault = sim_fault_once;
    sim_fault_once = 0u;

    if (fault & SIM_FAULT_DROP_SAMPLE) {
        if (++phase >= SIM_SINE_PERIOD) { phase = 0u; }
    }

    volatile uint16_t *p = &sim_dst[half * sim_half_len];
    const bool flat = (adc_pinsel() == SIM_FLAT_PINSEL);
    for (uint32_t i = 0; i < sim_half_len; i++) {
        p[i] = flat ? 3840u : sim_sine[phase];
        if (++phase >= SIM_SINE_PERIOD) { phase = 0u; }
    }

    uint32_t st = half ? DMA0_DONE : DMA0_HALF;
    st |= fault & 0x3FFu;
    half ^= 1u;

    dma0_event(st);
}

/* ------------------------------------------------------------------ *
 * Ping-pong check
 *
 * The stand-in writes a known vector, so the consumer side can be held
 * to it exactly: every sample of a completed half, as process_buffer()
 * receives it through capture_completed_half(), must equal the sine
 * table, and the phase must continue from where the previous half
 * ended. A half of 1024 samples advances the phase by 1024 mod 40 = 24,
 * so a half served twice (phase step 0) or two halves swapped (step 8
 * instead of 24) shows up as a mismatch at index 0, and a wrong pointer
 * or a corrupted region shows up at the index where it starts.
 *
 * Runs over SIM_CHECK_HALVES halves (SIM_CHECK_HALVES / 2 full DMA
 * buffers, i.e. that many HALF/DONE round trips), reports the first
 * mismatches and a verdict, then stops so the UART stays quiet. The
 * self-test input (flat 3840) is not part of the check; the check locks
 * on to the phase again at the next sine half. Results are also in
 * sim_check_halves / sim_check_bad / sim_check_done for MDB.
 * ------------------------------------------------------------------ */
#define SIM_CHECK_HALVES  100u                 /* 50 full buffers          */
#define SIM_CHECK_REPORT  2u                   /* bad halves to detail     */

volatile uint32_t sim_check_halves = 0;        /* sine halves compared     */
volatile uint32_t sim_check_bad    = 0;        /* halves with a mismatch   */
volatile uint32_t sim_check_done   = 0;        /* 1 once the verdict is out*/
static uint32_t   sim_check_phase  = SIM_SINE_PERIOD;   /* >= PERIOD: unknown */

bool sim_check_running(void)
{
    return sim_check_done == 0u;
}

/* Table index k with sim_sine[k] == a and sim_sine[k+1] == b, or
 * SIM_SINE_PERIOD if the pair is not two consecutive table entries. Two
 * samples are needed: a single value occurs twice per period. */
static uint32_t sim_phase_of(uint16_t a, uint16_t b)
{
    for (uint32_t k = 0; k < SIM_SINE_PERIOD; k++) {
        if ((sim_sine[k] == a) && (sim_sine[(k + 1u) % SIM_SINE_PERIOD] == b)) {
            return k;
        }
    }
    return SIM_SINE_PERIOD;
}

void sim_check_half(const volatile uint16_t *b, uint32_t n)
{
    if (sim_check_done) {
        return;
    }
    if ((b[0] == 3840u) && (b[1] == 3840u)) {  /* self-test input        */
        sim_check_phase = SIM_SINE_PERIOD;
        return;
    }

    uint32_t k = sim_check_phase;
    if (k >= SIM_SINE_PERIOD) {                /* first half: lock on     */
        k = sim_phase_of(b[0], b[1]);
    }

    uint32_t bad_at = n;
    if (k >= SIM_SINE_PERIOD) {
        bad_at = 0u;                           /* not even a sine start   */
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (b[i] != sim_sine[k]) { bad_at = i; break; }
            if (++k >= SIM_SINE_PERIOD) { k = 0u; }
        }
    }

    if (bad_at < n) {
        sim_check_bad++;
        if (sim_check_bad <= SIM_CHECK_REPORT) {
            console_kv("[simtest] mismatch in half", sim_check_halves);
            console_kv("[simtest]   index", bad_at);
            console_kv("[simtest]   got", b[bad_at]);
            console_kv("[simtest]   expected", (sim_check_phase < SIM_SINE_PERIOD)
                       ? sim_sine[(sim_check_phase + bad_at) % SIM_SINE_PERIOD] : 0u);
        }
        sim_check_phase = SIM_SINE_PERIOD;     /* re-lock on the next half*/
    } else {
        sim_check_phase = k;                   /* phase of the next sample*/
    }

    if (++sim_check_halves >= SIM_CHECK_HALVES) {
        sim_check_done = 1u;
        console_kv("[simtest] halves compared against the sine vector", sim_check_halves);
        console_kv("[simtest] full ping-pong buffers", sim_check_halves / 2u);
        console_kv("[simtest] halves with a mismatch", sim_check_bad);
        console_puts(sim_check_bad ? "[simtest] FAIL\r\n" : "[simtest] PASS: ping-pong order and data intact\r\n");
    }
}

/*
 * bench.c - the back-to-back test suite: "sweep" and "test" (bench.h)
 *
 * P6.2: moved verbatim out of cli.c (test_*, matrix_*, sweep_*, their
 * helpers and static state), together with the "sweep" and "test"
 * commands that drive them. Nothing in these functions changed - see
 * bench.h for why registration is split into two functions instead of
 * one, and cli.c for the four small reply helpers (put_kv(), put_line(),
 * arg_u32(), usage()) and run_dactest() this file still borrows from it
 * rather than duplicating: they are declared extern below and defined,
 * non-static since this move, in cli.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "bench.h"
#include "capture.h"
#include "clock.h"
#include "timebase.h"
#include "dac.h"
#include "uart.h"
#include "chaintest.h"
#include "console.h"
#include "cmd_parser.h"
#include "fmt.h"
#include "sim.h"

/* cli.c - the parser reply helpers and the DAC/UREF routing for the
 * "dactest" command, shared with cmd_dactest_fn() there. */
void put_kv(const char *key, uint32_t v);
void put_line(const char *s);
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out);
void usage(const char *text);
uint32_t run_dactest(uint32_t bursts);

/* ------------------------------------------------------------------ *
 * sweep - the rate measurement, automated
 *
 * For each divide ratio of the ladder (capture.c), slowest rate first,
 * three runs of `halves` halves: the CPU idle, the CPU doing the
 * main-loop processing, and the CPU polling an SFR. The counters after
 * each run say whether the DMA kept up (overrun) and whether the CPU
 * got every half (missed). The clock is switched once per row and read
 * back; a row whose switch did not arrive prints that and no numbers.
 * Three times per rate, with the CPU doing something different while
 * the DMA runs, because the first board run could not tell whether the
 * overruns come from the DMA bus itself or from the CPU competing for
 * it (DS70005591D 13.4.4, one shared DMA data bus):
 *
 *   idle     the CPU polls blocks_done, a RAM variable, nothing else
 *   process  the CPU runs capture_service(), i.e. process_buffer() on
 *            every completed half - what the application would do
 *   sfr      the CPU probes the UART's status register in a tight loop
 *            (uart_stat_probe()) - the worst case, a CPU that hammers
 *            the peripheral bus (the console does this while it prints)
 *
 * One line per rate. overrun must be 0 for a rate to be usable. The
 * whole sweep runs inside the receive interrupt, like every command;
 * the DMA interrupt preempts it, the main loop is starved meanwhile
 * (counters are cleared afterwards, so that does not show up). The
 * previous sample time and run state are restored at the end.
 * ------------------------------------------------------------------ */
#define SWEEP_HALVES_DEFAULT  2000u          /* 2 M samples per point   */
#define SWEEP_WAIT_LIMIT      400000000u     /* loop iterations, ~10 s  */

/* The MEASURED rate comes from timebase.c (Timer1 at 12.5 MHz). The rate
 * the first board sweep printed was the nominal 40/(SAMC+1); the counters
 * said it was not what the ADC did (equal overruns at every "rate",
 * halves missed at 1.25 MSPS), so from then on the sweep measures. */

enum sweep_load { SWEEP_IDLE = 0, SWEEP_PROCESS = 1, SWEEP_SFR = 2 };

/* Run `halves` halves at `samc` with the CPU under `load` meanwhile.
 * *ticks receives the Timer1 ticks the halves took. Returns false if
 * the stream stopped or never delivered. */
static bool sweep_point(uint32_t halves, enum sweep_load load, uint32_t *ticks)
{
    uint32_t n = SWEEP_WAIT_LIMIT;
    (void)capture_settle();                                   /* defined start     */
    counters_clear();
    const uint32_t target = blocks_done + halves;
    const uint32_t t0 = timebase_ticks();
    capture_start();
    n = SWEEP_WAIT_LIMIT;
    while (blocks_done < target) {
        SIM_DMA_TICK();
        if (load == SWEEP_PROCESS)      { (void)capture_service(); }
        else if (load == SWEEP_SFR)     { uart_stat_probe(); }
        /* The brake fired: this rate floods the CPU with overrun
         * interrupts and is unusable. Not an error of the point. */
        if (capture_overrun_aborted()) { (void)capture_settle(); return false; }
        if (--n == 0u) { (void)capture_settle(); return false; }
    }
    *ticks = timebase_ticks() - t0;   /* unsigned: wrap-safe             */
    (void)capture_settle();           /* point over: DMA down            */
    return true;
}

/* Returns true if the process run delivered every half with no overrun. */
static bool sweep_row(struct pll_step st, uint32_t halves)
{
    /* The clock is switched once for the row, not once per load: the
     * three loads differ in what the CPU does, not in the rate. If the
     * switch does not arrive, the row says so and no number is printed -
     * a measured rate under an unknown divider is what made run 7
     * unreadable. */
    const uint32_t rc = capture_set_pll(st.p1, st.p2);
    if (rc != CLKDIV_OK) {
        /* Two single digits plus the longest error text of about 50:
         * well inside 144. */
        char bad[144];
        char *q = copy_str(bad, "postdiv ");  q = u32_to_str(q, st.p1);
        *q++ = '/';                           q = u32_to_str(q, st.p2);
        q = copy_str(q, ": clock switch FAILED - ");
        q = copy_str(q, clock_adc_div_error(rc));
        copy_str(q, "\r\n");
        console_puts(bad);
        return false;
    }

    /* The rate, measured on ONE burst with nothing else running. The
     * three loaded runs below cannot measure it: at a rate that overruns,
     * the CPU drowns in the overrun interrupt and the loaded figure came
     * out ten times too high at every setting (runs 8 to 11 reported
     * 42 MSPS everywhere, while a clean burst at the same setting
     * measured 3990 ksps against 4081 nominal - run 13). Both are printed,
     * because the difference is the artefact. */
    uint32_t clean_ksps = 0u, clean10_ksps = 0u;
    {
        const uint32_t nn = 2u * capture_half_len();
        /* Ten bursts as well as one. In run 16 a single burst delivered
         * the rate the PLL was set to and a thousand delivered 40 MSPS at
         * every setting; ten bursts sit between the two and say which of
         * them a burst in a stream resembles. */
        if (capture_oneshot_n(10u) == 0u) {
            clean10_ksps = timebase_ksps(10u * nn, capture_oneshot_ticks());
        }
        (void)capture_settle();
        if (capture_oneshot() == 0u) {
            /* The burst alone - capture_oneshot_ticks() excludes the DMA
             * teardown and setup, whose fixed 11.3 us used to make every
             * rate read low, by 2.2 % at 4 MSPS and 17.8 % at 40 (run 14). */
            clean_ksps = timebase_ksps(nn, capture_oneshot_ticks());
        }
        (void)capture_settle();
    }

    uint32_t ov[3], ticks[3] = { 0, 0, 0 };
    bool     ok[3];
    uint32_t late = 0, missed = 0;
    for (int l = 0; l < 3; l++) {
        ok[l] = sweep_point(halves, (enum sweep_load)l, &ticks[l]);
        ov[l] = dma_overrun;
        if (l == SWEEP_PROCESS) { late = late_service; missed = proc_missed; }
    }
    /* Measured rate of the idle run. */
    const uint32_t meas_ksps = ok[0] ? timebase_ksps(halves * capture_half_len(), ticks[0]) : 0u;
    /* Longest line: 150 characters plus NUL; every number is at most
     * 10 digits, "STOPPED" is shorter. */
    char line[208];
    char *p = copy_str(line, "postdiv ");      p = u32_to_str(p, st.p1);
    *p++ = '/';                                p = u32_to_str(p, st.p2);
    p = copy_str(p, "  adc clock Hz ");        p = u32_to_str(p, clock_adc_hz());
    p = copy_str(p, "  ksps nom ");            p = u32_to_str(p, capture_nominal_ksps(0u));
    p = copy_str(p, " clean1 ");               p = u32_to_str(p, clean_ksps);
    p = copy_str(p, " clean10 ");              p = u32_to_str(p, clean10_ksps);
    p = copy_str(p, " loaded ");               p = u32_to_str(p, meas_ksps);
    p = copy_str(p, "  overrun idle/process/sfr ");
    for (int l = 0; l < 3; l++) {
        if (l) { *p++ = '/'; }
        p = ok[l] ? u32_to_str(p, ov[l]) : copy_str(p, "STOPPED");
    }
    p = copy_str(p, "  late ");               p = u32_to_str(p, late);
    p = copy_str(p, "  missed ");             p = u32_to_str(p, missed);
    copy_str(p, "\r\n");
    console_puts(line);               /* blocking: works from main() too */

    /* The three numbers that answer why there are overruns at 4 MSPS,
     * where the DMA has eight times the headroom it needs, and why the
     * loaded rate reads ten times the clean one. From the process run.
     * half + done must equal the halves counted; if half is of the order
     * of the overrun count instead, the status flags are not clearing and
     * the handler is booking the same event over and over - our bug, not
     * the silicon's. */
    /* blocks_done is NOT reset by counters_clear() - it is free running,
     * so printing it raw next to per-point counters compared a total
     * against a sample and made the relation unreadable (run 16). The
     * delta over this point is what belongs beside them. */
    char c2[144];
    char *q = copy_str(c2, "[sweep]   isr ");  q = u32_to_str(q, isr_entries);
    q = copy_str(q, "  half ");                q = u32_to_str(q, half_events);
    q = copy_str(q, "  done ");                q = u32_to_str(q, done_events);
    q = copy_str(q, "  bursts ");             q = u32_to_str(q, burst_starts);
    q = copy_str(q, "  half+done ");          q = u32_to_str(q, half_events + done_events);
    q = copy_str(q, " (must be 2x bursts)");
    copy_str(q, "\r\n");
    console_puts(c2);
    return ok[SWEEP_PROCESS] && (ov[SWEEP_PROCESS] == 0u) && (missed == 0u) && (late == 0u);
}

/* No longer declared in console.h: cmd_sweep_fn() and test_sweep(), its
 * only two callers, both moved here with it. */
static void console_sweep(uint32_t halves, bool choose)
{
    uint32_t count = 0;
    const struct pll_step *steps = capture_sweep_steps(&count);  /* slowest rate first */
    const bool was_running = capture_running();

    console_kv("[sweep] halves per point", halves);
    /* Without this the table cannot be read afterwards. Every rate and
     * every duration derived from a sweep row is (halves x samples per
     * half) divided by a rate, so an evaluation that assumes 1024 while
     * the run used something else is wrong without looking wrong. "buf"
     * can change it at run time, so it has to be in the log. */
    console_kv("[sweep] samples per half", capture_half_len());
    console_kv("[sweep] samples per point", halves * capture_half_len());
    console_puts("[sweep] back-to-back conversions; the rate is the ADC clock, PLL1 output dividers\r\n"
                 "[sweep] slowest rate first: the first row is the one the DMA should manage,\r\n"
                 "[sweep] so a failure there is the chain, not the rate\r\n"
                 "[sweep] idle = CPU polls RAM only, process = main-loop processing, sfr = CPU polls an SFR\r\n"
                 "[sweep] overrun must be 0 for a usable rate; late/missed are from the process run\r\n"
                 "[sweep] postdiv = PLL1 POSTDIV1/POSTDIV2; the adc clock is read back from the registers\r\n"
                 "[sweep] clean1/clean10 = rate over one burst and over ten, nothing else running;\r\n"
                 "[sweep] loaded = rate while the three runs below are going\r\n"
                 "[sweep] the three runs below are going. Trust clean: the loaded figure is\r\n"
                 "[sweep] measured by a CPU drowning in overrun interrupts\r\n");

    /* Time base check: 100 ms of CPU time (200 MHz) must be 1 250 000
     * ticks. Anything else and the measured rates are off by the same
     * factor - and the assumption about the timer's clock is wrong. */
    console_kv("[sweep] timer check, ticks per 100 ms (expect 1250000)", timebase_check());

    struct pll_step best = { 0u, 0u };
    uint32_t best_ksps = 0u;              /* fastest clean row           */
    uint32_t clean = 0u, rows = 0u;
    for (uint32_t i = 0; i < count; i++) {
        console_puts("[sweep] ");
        rows++;
        if (sweep_row(steps[i], halves)) {
            clean++;
            const uint32_t k = capture_nominal_ksps(0u);
            if (k > best_ksps) { best = steps[i]; best_ksps = k; }
        }
    }
    console_kv("[sweep] rows", rows);
    console_kv("[sweep] rows with overrun 0, late 0 and missed 0", clean);

    /* The question the sweep answers: the highest rate at which the CPU
     * gets every half (missed 0) and the DMA every sample (overrun 0),
     * with the main-loop processing running. A rate with overruns also
     * raises the DMA interrupt for every lost sample (1.6 million per
     * second at 40 MSPS on the board) and starves the console. */
    if (best_ksps != 0u) {
        console_kv("[sweep] highest clean rate, ksps", best_ksps);
        console_kv("[sweep]   at POSTDIV1", best.p1);
        console_kv("[sweep]   at POSTDIV2", best.p2);
        if (choose) { (void)capture_set_pll(best.p1, best.p2); }
    } else {
        console_puts("[sweep] NO CLEAN RATE - every row lost samples or halves\r\n");
    }
    if (!choose) { (void)capture_set_pll(ADC_PLL_POSTDIV1, ADC_PLL_POSTDIV2); }
    counters_clear();
    if (was_running) { capture_start(); }
    console_puts("[sweep] done: counters cleared\r\n");
}

static void cmd_sweep_fn(int argc, char **argv)
{
    uint32_t halves = SWEEP_HALVES_DEFAULT;
    if ((argc > 2) || ((argc == 2) && !arg_u32(argv[1], 10u, 100000u, &halves))) {
        usage("sweep [halves per point 10..100000, default 2000]");
        return;
    }
    console_sweep(halves, false);
}
CMD_DEFINE(sweep, "sweep", cmd_sweep_fn, "sweep [halves] - overrun vs sample rate over the clock ladder");

void bench_register_sweep(void)
{
    (void)cmd_register(&cmd_sweep);
}

/* ------------------------------------------------------------------ *
 * "test" - the parts of a run, one command
 *
 * The firmware does nothing on its own any more: it boots, brings the
 * console up and waits. The colleague at the board first proves that the
 * console works at all (anything typed echoes, "help" answers), then
 * runs the parts of the test from here. Every part ends in exactly one
 * PASS or FAIL line so that the log stays readable.
 *
 * Order in "test all", and the reason for it:
 *   self   is the chain intact?  ADC -> DMA -> RAM on the internal
 *          reference, at the slowest clock. If this fails nothing after
 *          it means anything, so "all" stops here and only here.
 *   clock  does a divider change arrive in the hardware? Every ratio is
 *          switched and read back, nothing is measured. This separates
 *          "the switch does not happen" from "the switch happens and the
 *          rate does not follow" - the question run 7 left open.
 *   sweep  where is the limit? The ladder from the slowest rate up, with
 *          overrun, late and missed per point. This is the table the
 *          customer gets.
 *   dac    does everything arrive, in order? The DAC2 triangle through
 *          the same chain. Counters cannot show a swapped or skipped
 *          half; a known signal can.
 * ------------------------------------------------------------------ */
#define TEST_SWEEP_HALVES   2000u
#define TEST_RATE_HALVES    200u
#define TEST_DAC_HALVES     64u

static bool test_self(void)
{
    uint32_t mean = 0;
    console_puts("[test] self: ADC -> DMA -> RAM on the internal 15/16 VDD reference\r\n");
    const uint32_t rc = capture_selftest(&mean);
    console_kv("[test]   mean (expect 3648..4032)", mean);
    console_puts((rc == 0u) ? "[test] self: PASS\r\n" : "[test] self: FAIL\r\n");
    if (rc == 6u)      { console_puts("[test]   no data - nothing converted\r\n"); }
    else if (rc == 7u) { console_puts("[test]   mean outside the window\r\n"); }
    else if (rc == 8u) { console_puts("[test]   the DMA channel switched itself off\r\n"); }
    return rc == 0u;
}

/* The CLKGEN6 divide ratios, for "test clock" only: this is the knob
 * that writes and reads back correctly and does not change the rate
 * (HARDWARE-LOG runs 8 and 9). The rate ladder lives in capture.c and is
 * made of PLL settings. Ratios between 100 and 200 are left out - FRACDIV
 * does nothing while INTDIV is 0, so they cannot be realised. */
static const uint32_t clkdiv_ratios[] = {
    1000u, 900u, 800u, 700u, 600u, 500u, 450u, 400u, 350u, 300u, 250u, 200u, 100u
};

static bool test_clock(void)
{
    const uint32_t count  = sizeof clkdiv_ratios / sizeof clkdiv_ratios[0];
    const uint32_t *ratios = clkdiv_ratios;
    uint32_t bad = 0u;
    console_puts("[test] clock: switch every divide ratio and read it back - no measurement\r\n"
                 "[test]   order per ratio: DMA down, ADC core off, CLKGEN6 off, divider\r\n"
                 "[test]   written and read back, generator on, DIVSWEN, CLKRDY, core on\r\n");
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t rc   = capture_set_clkdiv(ratios[i]);
        const uint32_t back = capture_clkdiv();
        /* Longest line: ratio and read-back (4 digits each), the clock in
         * Hz (9 digits) and the longest error text (about 50), well
         * inside 160. */
        char line[160];
        char *q = copy_str(line, "[test]   ratio ");  q = u32_to_str(q, ratios[i]);
        q = copy_str(q, " -> read back ");            q = u32_to_str(q, back);
        q = copy_str(q, ", adc clock Hz ");           q = u32_to_str(q, clock_adc_hz());
        q = copy_str(q, ", ");                        q = copy_str(q, clock_adc_div_error(rc));
        if ((rc == CLKDIV_OK) && (back != ratios[i])) {
            q = copy_str(q, " - BUT THE READBACK DIFFERS");
        }
        copy_str(q, "\r\n");
        console_puts(line);
        if ((rc != CLKDIV_OK) || (back != ratios[i])) { bad++; }
    }
    (void)capture_set_clkdiv(ADC_CLKDIV);
    console_kv("[test]   ratios that did not arrive", bad);
    console_puts("[test]   NOTE: this only proves the register holds the value. Runs 8 and 9\r\n"
                 "[test]   passed here and the rate did not follow at any ratio - the rate\r\n"
                 "[test]   is set with the PLL now (test sweep, pll command).\r\n");
    console_puts((bad == 0u) ? "[test] clock: PASS\r\n" : "[test] clock: FAIL\r\n");
    return bad == 0u;
}

static bool test_clkoff(void)
{
    /* Is CLKGEN6 the ADC's clock at all? Table 16-1 says so, and the
     * generator's divider has no effect on the rate - so ask the board. */
    console_puts("[test] clkoff: switch CLKGEN6 off and try to convert anyway\r\n"
                 "[test]   Table 16-1 names CLKGEN6 as the ADC clock, but its divider\r\n"
                 "[test]   changes nothing. If halves still arrive with the generator\r\n"
                 "[test]   off, the ADC is not running off it and that explains it all.\r\n");
    const uint32_t rc = capture_clkoff_probe(8u);
    console_kv("[clkoff]   halves after the generator was switched off", blocks_done);
    if (rc == 0u) {
        console_puts("[test] clkoff: THE ADC KEPT CONVERTING WITH CLKGEN6 OFF\r\n"
                     "[test]   -> CLKGEN6 is not (only) the ADC clock; the rate must come\r\n"
                     "[test]      from somewhere else. This is the finding, not a failure.\r\n");
        return false;
    }
    console_puts("[test] clkoff: no data with the generator off - CLKGEN6 does feed the ADC\r\n");
    return true;
}

static bool test_rate(uint32_t halves)
{
    uint32_t ksps = 0u;
    const uint32_t ratio   = capture_clkdiv();
    const uint32_t nominal = capture_nominal_ksps(ratio);
    console_puts("[test] rate: delivered rate at the ratio set now\r\n");
    console_kv("[test]   ratio", ratio);
    console_kv("[test]   ksps nominal", nominal);
    const uint32_t rc = capture_measure_rate(halves, &ksps);
    if (rc != 0u) {
        console_puts("[test]   no data\r\n[test] rate: FAIL\r\n");
        return false;
    }
    console_kv("[test]   ksps measured", ksps);
    console_kv("[test]   overrun during the measurement", dma_overrun);
    /* 10 % is the window the old rate test used; the rate comes from a
     * divided clock, so anything outside it means the divider is not
     * doing what the register says. */
    const uint32_t diff = (ksps > nominal) ? (ksps - nominal) : (nominal - ksps);
    const bool ok = (nominal != 0u) && (diff <= (nominal / 10u));
    console_puts(ok ? "[test] rate: PASS\r\n"
                    : "[test] rate: FAIL - the delivered rate does not follow the ratio\r\n");
    return ok;
}

static bool test_sweep(uint32_t halves)
{
    console_puts("[test] sweep: the ladder from the slowest rate up\r\n");
    console_sweep(halves, false);
    console_puts("[test] sweep: done - read the table, there is no single verdict\r\n");
    return true;
}

static bool test_dac(uint32_t halves)
{
    console_puts("[test] dac: the DAC2 triangle through ADC, DMA and the ping-pong buffer\r\n");
    /* SLPDAT is the step per DAC clock, so a LARGER value is a FASTER
     * triangle. 8 leaves the signal almost standing still inside one
     * captured buffer - run 12 measured a swing of 72 counts and the test
     * could say nothing. 64 puts a full period in the window, which is
     * what run 13 then showed as a triangle. */
    if (!dac_running(2u) && !dac_triangle_start(2u, 0x100u, 0xF00u, 64u)) {
        console_puts("[test]   CLKGEN7 did not come up - DAC2 is off\r\n[test] dac: FAIL\r\n");
        return false;
    }
    const uint32_t rc = run_dactest(halves);
    console_puts((rc == 0u) ? "[test] dac: PASS\r\n" : "[test] dac: FAIL\r\n");
    return rc == 0u;
}

/* ------------------------------------------------------------------ *
 * "test matrix" - every documented way to set the sample rate, tried
 *
 * Four of these were tried before and written off as "does not work".
 * For three of them the reason turned out to be in our own code: the ADC
 * trigger number selected a different SCCP module, the auxiliary output
 * carried the timer rollover instead of the special event trigger, and
 * the trigger module was clocked from a different PLL than the ADC. Since
 * the documentation has been wrong about this device twice, every
 * combination is asked of the board rather than reasoned about.
 *
 * Each variant answers the same four questions, with the instruments that
 * have earned trust:
 *   converts      does a burst produce halves at all - bounded, so a
 *                 variant that does nothing costs milliseconds
 *   rate follows  the delivered rate from ONE CLEAN BURST timed with
 *                 Timer1, never under load. This is where eleven runs
 *                 went wrong: measured while the CPU drowned in the
 *                 overrun interrupt, every rate came out ten times high
 *   xfer/trigger  transfers per trigger = 2048 / (window / trigger
 *                 period). Microchip acknowledges "a few transfers are
 *                 possible per one trigger" on this silicon; this is the
 *                 direct measurement of it
 *   data intact   the DAC triangle through the chain, for the variants
 *                 that got that far - run last and only for those, to
 *                 keep the log readable
 * ------------------------------------------------------------------ */
#define MATRIX_POINTS   3u
static const uint32_t matrix_ksps[MATRIX_POINTS] = { 4000u, 8000u, 20000u };
#define MATRIX_TOL_PCT  10u
/* The acceptance test's two points and its length. The slow one is where
 * a variant has the best chance; 8000 is the rate the example is meant to
 * be shown at. 2000 halves is half a second at 4 MSPS - long enough that
 * a stream which only looks clean for a moment does not pass. */
#define MATRIX_STREAM_SLOW    4000u
#define MATRIX_STREAM_TARGET  8000u
#define MATRIX_STREAM_HALVES  2000u

struct matrix_result {
    bool converts;
    bool rate_follows;
    bool streams;        /* THE question: a lasting stream, CPU keeping up */
    bool checked;        /* data intact, from the DAC triangle            */
    uint32_t stream_ksps;
};

/* ------------------------------------------------------------------ *
 * The acceptance test of the example, and until now it was missing
 *
 * What this project is supposed to demonstrate is one sentence: at a
 * rate you choose, samples stream into RAM through the DMA and the CPU
 * processes them on the free half. The matrix measured around that -
 * does it convert, does the rate follow, are the data intact - and never
 * asked the sentence itself. So a variant could pass everything and
 * still be useless for the example.
 *
 * This is the sentence as a test: run the stream for `halves` halves
 * with capture_service() called the whole time, exactly as the main loop
 * does, and require all three counters to stay at zero. Overrun means
 * samples were lost. Late means the handler was more than one half
 * behind. Missed means the CPU never saw a half - which is precisely the
 * failure the example must not have, because processing on the free half
 * is the entire point.
 * ------------------------------------------------------------------ */
static bool matrix_stream(capture_variant_t v, uint32_t want, uint32_t halves,
                          uint32_t *got_ksps)
{
    if (got_ksps != NULL) { *got_ksps = 0u; }
    if (!capture_select_variant(v, want)) { return false; }

    (void)capture_settle();
    counters_clear();
    const uint32_t target = blocks_done + halves;
    const uint32_t t0     = timebase_ticks();
    capture_start();
    uint32_t guard = SWEEP_WAIT_LIMIT;
    while (blocks_done < target) {
        (void)capture_service();          /* the main loop's own work    */
        if (capture_overrun_aborted()) { break; }
        if (--guard == 0u) { break; }
    }
    const uint32_t ticks = timebase_ticks() - t0;
    const uint32_t ov = dma_overrun, la = late_service, mi = proc_missed;
    const uint32_t done = blocks_done;
    (void)capture_settle();

    const uint32_t ksps = timebase_ksps(halves * capture_half_len(), ticks);
    if (got_ksps != NULL) { *got_ksps = ksps; }

    char line[176];
    char *q = copy_str(line, "[matrix]   stream at ");  q = u32_to_str(q, want);
    q = copy_str(q, " ksps: measured ");                q = u32_to_str(q, ksps);
    q = copy_str(q, "  overrun ");                      q = u32_to_str(q, ov);
    q = copy_str(q, "  late ");                         q = u32_to_str(q, la);
    q = copy_str(q, "  missed ");                       q = u32_to_str(q, mi);
    copy_str(q, "\r\n");
    console_puts(line);

    if (done < target) {
        console_puts(capture_overrun_aborted()
                     ? "[matrix]     stopped by the overrun brake - unusable\r\n"
                     : "[matrix]     the stream did not deliver - unusable\r\n");
        return false;
    }
    const bool clean = (ov == 0u) && (la == 0u) && (mi == 0u);
    console_puts(clean ? "[matrix]     CLEAN - nothing lost and the CPU kept up\r\n"
                       : "[matrix]     not clean - see the three counters\r\n");
    return clean;
}

/* One rate point: select, run one clean burst, report. */
static bool matrix_point(capture_variant_t v, uint32_t want, bool show_regs)
{
    if (!capture_select_variant(v, want)) {
        console_kv("[matrix]   could not configure for ksps", want);
        return false;
    }
    if (show_regs) { capture_variant_regs(); }

    const uint32_t nominal = capture_variant_ksps();
    const uint32_t n     = 2u * capture_half_len();
    const uint32_t rc    = capture_oneshot();
    const uint32_t ticks = capture_oneshot_ticks();   /* the burst alone */
    (void)capture_settle();

    if (rc != 0u) {
        console_kv("[matrix]   ksps asked", want);
        console_puts("[matrix]     NO DATA - nothing converted\r\n");
        return false;
    }
    const uint32_t ksps = timebase_ksps(n, ticks);
    const uint32_t diff = (ksps > nominal) ? (ksps - nominal) : (nominal - ksps);
    const bool     ok   = (nominal != 0u) && (diff <= (nominal * MATRIX_TOL_PCT / 100u));

    /* Longest line: four numbers of at most 10 digits and about 70
     * characters of text, well inside 160. */
    char line[160];
    char *q = copy_str(line, "[matrix]   asked ");  q = u32_to_str(q, want);
    q = copy_str(q, "  nominal ");                  q = u32_to_str(q, nominal);
    q = copy_str(q, "  measured ");                 q = u32_to_str(q, ksps);
    q = copy_str(q, " ksps  -> ");
    q = copy_str(q, ok ? "follows" : "DOES NOT FOLLOW");
    copy_str(q, "\r\n");
    console_puts(line);

    /* Transfers per trigger, for the triggered variants: the trigger
     * period is exact, the transfer count is exactly one buffer, and the
     * window came from Timer1. */
    const uint32_t trig_ns = capture_trigger_period_ns();
    if ((trig_ns != 0u) && (ticks != 0u)) {
        const uint32_t window_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / TIMEBASE_HZ);
        const uint32_t triggers  = window_ns / trig_ns;
        if (triggers != 0u) {
            console_kv("[matrix]     triggers in the window", triggers);
            console_kv("[matrix]     transfers per trigger x100", (n * 100u) / triggers);
        }
    }
    console_kv("[matrix]     overrun during the burst", dma_overrun);
    return ok;
}

static void cmd_matrix(void)
{
    struct matrix_result res[CAP_VAR_COUNT];
    console_puts("\r\n[matrix] every documented way to set the sample rate, tried on the board\r\n"
                 "[matrix] rate measured on one clean burst per point, never under load\r\n"
                 "[matrix] see sccp.h for the three errors this replaces\r\n");

    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        res[v].converts = false;
        res[v].rate_follows = false;
        res[v].streams = false;
        res[v].checked = false;
        res[v].stream_ksps = 0u;

        console_puts("\r\n[matrix] ");
        console_puts(capture_variant_name((capture_variant_t)v));
        console_puts("\r\n");

        uint32_t good = 0u;
        for (uint32_t i = 0; i < MATRIX_POINTS; i++) {
            if (matrix_point((capture_variant_t)v, matrix_ksps[i], i == 0u)) { good++; }
            if (blocks_done != 0u) { res[v].converts = true; }
        }
        res[v].rate_follows = (good == MATRIX_POINTS);
        console_puts(res[v].rate_follows
                     ? "[matrix]   VERDICT: the rate follows at every point\r\n"
                     : "[matrix]   VERDICT: not usable as a rate control\r\n");
    }

    /* ---- the acceptance test: does it stream, with the CPU keeping up?
     * Run for every variant that converted at all, not only for those
     * whose rate followed - a variant could stream cleanly at a rate
     * that is not the one asked for, and that is worth knowing. Two
     * points: the slowest, where it is most likely to work, and 8 MSPS. */
    console_puts("\r\n[matrix] THE ACCEPTANCE TEST: a lasting stream with the CPU processing\r\n"
                 "[matrix] overrun, late and missed must all be zero - missed above zero means\r\n"
                 "[matrix] the CPU never saw a half, which is the whole point of the example\r\n");
    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        if (!res[v].converts) { continue; }
        console_puts("\r\n[matrix] ");
        console_puts(capture_variant_name((capture_variant_t)v));
        console_puts("\r\n");
        uint32_t k = 0u;
        const bool slow = matrix_stream((capture_variant_t)v, MATRIX_STREAM_SLOW,
                                        MATRIX_STREAM_HALVES, &k);
        uint32_t k8 = 0u;
        const bool at8 = matrix_stream((capture_variant_t)v, MATRIX_STREAM_TARGET,
                                       MATRIX_STREAM_HALVES, &k8);
        res[v].streams     = slow || at8;
        res[v].stream_ksps = at8 ? k8 : k;
    }

    /* The data check, only for what streams - a clean stream that
     * carries the wrong samples would be the worst outcome of all. */
    console_puts("\r\n[matrix] data check on the variants that streamed cleanly\r\n");
    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        if (!res[v].streams) { continue; }
        console_puts("[matrix] ");
        console_puts(capture_variant_name((capture_variant_t)v));
        console_puts("\r\n");
        if (capture_select_variant((capture_variant_t)v, 8000u)) {
            /* Once as an isolated burst and once as the hundredth of a
             * stream. The triangle's period cannot change, so period in
             * samples divided by rate must agree; if it does not, the
             * samples in the stream are repeats. */
            console_puts("[matrix]   isolated burst:\r\n");
            const bool one = (run_dactest(1u) == 0u);
            console_puts("[matrix]   the 100th burst of a stream:\r\n");
            const bool hundred = (run_dactest(100u) == 0u);
            res[v].checked = one && hundred;
        }
    }

    console_puts("\r\n[matrix] SUMMARY - the last column is the one the example lives on\r\n");
    uint32_t winner = (uint32_t)CAP_VAR_COUNT;
    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        char line[176];
        char *q = copy_str(line, "[matrix]   ");
        q = copy_str(q, res[v].rate_follows ? "RATE OK  " : "         ");
        q = copy_str(q, res[v].checked      ? "DATA OK  " : "         ");
        q = copy_str(q, res[v].streams      ? "STREAMS  " : "         ");
        q = copy_str(q, capture_variant_name((capture_variant_t)v));
        copy_str(q, "\r\n");
        console_puts(line);
        if (res[v].streams && res[v].checked && (winner == (uint32_t)CAP_VAR_COUNT)) {
            winner = v;
        }
    }
    if (winner != (uint32_t)CAP_VAR_COUNT) {
        console_puts("[matrix] USE THIS ONE: ");
        console_puts(capture_variant_name((capture_variant_t)winner));
        console_puts("\r\n[matrix]   it streams without losing a sample, the CPU keeps up,\r\n"
                     "[matrix]   and the data arrive complete and in order\r\n");
    } else {
        console_puts("[matrix] NONE of the variants streams cleanly with the CPU keeping up.\r\n"
                     "[matrix]   That is the example's central claim, so until one does, the\r\n"
                     "[matrix]   example does not demonstrate what it says it demonstrates.\r\n");
    }
    /* Back to a known state: the boot setting. */
    (void)capture_select_variant(CAP_VAR_B2B, capture_nominal_ksps(0u));
    (void)capture_set_pll(ADC_PLL_POSTDIV1, ADC_PLL_POSTDIV2);
    console_puts("[matrix] done - back at the boot setting\r\n");
}

/* ------------------------------------------------------------------ *
 * "test bursts" - does the rate depend on how many bursts run?
 *
 * The single measurement that names the contradiction of run 16. One
 * burst at a given setting delivers the rate that was asked for; a
 * thousand bursts at the same setting delivered about 40 MSPS whatever
 * the setting was. Nothing in between had ever been measured, so the two
 * observations sat next to each other with no bridge.
 *
 * capture_oneshot_n() builds the bridge: it restarts each burst from the
 * DMA interrupt exactly as continuous streaming does, and stops after
 * the count given. Reading the rate at one, ten and a hundred bursts at
 * the same clock setting says which of the two pictures a burst in a
 * stream belongs to - and if the rate climbs with the count, it says how
 * quickly.
 * ------------------------------------------------------------------ */
static void test_bursts(void)
{
    static const uint32_t counts[] = { 1u, 10u, 100u };
    static const uint32_t rates[]  = { 4000u, 8000u, 20000u };

    console_puts("[test] bursts: the same setting, measured over 1, 10 and 100 bursts\r\n"
                 "[test]   each burst is restarted from the DMA interrupt, as streaming does\r\n"
                 "[test]   a rate that climbs with the count is the bridge between run 16's\r\n"
                 "[test]   single burst (the setting) and its thousand (40 MSPS regardless)\r\n");

    for (uint32_t r = 0; r < (sizeof rates / sizeof rates[0]); r++) {
        if (!capture_select_variant(CAP_VAR_B2B, rates[r])) { continue; }
        const uint32_t nominal = capture_variant_ksps();
        const uint32_t n       = 2u * capture_half_len();
        console_kv("[test]   nominal ksps", nominal);
        for (uint32_t c = 0; c < (sizeof counts / sizeof counts[0]); c++) {
            const uint32_t rc = capture_oneshot_n(counts[c]);
            const uint32_t tk = capture_oneshot_ticks();
            (void)capture_settle();
            char line[144];
            char *q = copy_str(line, "[test]     bursts ");  q = u32_to_str(q, counts[c]);
            q = copy_str(q, ": ");
            if (rc != 0u) {
                q = copy_str(q, "no data");
            } else {
                q = copy_str(q, "ksps ");
                q = u32_to_str(q, timebase_ksps(counts[c] * n, tk));
                q = copy_str(q, "  overrun ");
                q = u32_to_str(q, dma_overrun);
            }
            copy_str(q, "\r\n");
            console_puts(line);
        }
    }
    console_puts("[test] bursts: done - equal rates mean a burst in a stream is an ordinary\r\n"
                 "[test]   burst; a rising rate means it is not, and the single-burst figures\r\n"
                 "[test]   measured so far describe a start-up rather than the stream\r\n");
}

static void test_list(void)
{
    put_line("test all   [halves]  - self, clock, sweep, dac in that order");
    put_line("test self            - ADC -> DMA -> RAM on the internal reference");
    put_line("test clock           - switch every CLKGEN6 ratio and read it back");
    put_line("test clkoff          - switch CLKGEN6 off: does the ADC still convert?");
    put_line("test matrix          - every way to set the rate, tried and measured");
    put_line("test bursts          - does the rate depend on how many bursts run?");
    put_line("test rate  [halves]  - delivered rate at the ratio set now");
    put_line("test sweep [halves]  - the rate ladder, slowest first, with the counters");
    put_line("test dac   [halves]  - the DAC2 triangle: is everything there, in order?");
    put_line("pll <p1> <p2> sets the rate by hand; clk sets the (ineffective) CLKGEN6");
    put_line("ratio; regs prints the registers");
}

static void cmd_test_fn(int argc, char **argv)
{
    uint32_t halves = 0u;                  /* 0 = the part's own default */
    if (argc == 1) { test_list(); return; }
    chain_stream_off();                    /* the tests need the boot setup */
    if (argc > 3) { usage("test <all|self|clock|clkoff|bursts|matrix|rate|sweep|dac> [n]"); return; }
    if ((argc == 3) && !arg_u32(argv[2], 1u, 100000u, &halves)) {
        usage("test <all|self|clock|clkoff|bursts|matrix|rate|sweep|dac> [n]");
        return;
    }
    const char *what = argv[1];

    if (strcmp(what, "self") == 0) {
        if (!test_self()) { cmd_parser_fail(); }
    } else if (strcmp(what, "clock") == 0) {
        if (!test_clock()) { cmd_parser_fail(); }
    } else if (strcmp(what, "clkoff") == 0) {
        if (!test_clkoff()) { cmd_parser_fail(); }
    } else if (strcmp(what, "matrix") == 0) {
        cmd_matrix();
    } else if (strcmp(what, "bursts") == 0) {
        test_bursts();
    } else if (strcmp(what, "rate") == 0) {
        if (!test_rate((halves != 0u) ? halves : TEST_RATE_HALVES)) { cmd_parser_fail(); }
    } else if (strcmp(what, "sweep") == 0) {
        (void)test_sweep((halves != 0u) ? halves : TEST_SWEEP_HALVES);
    } else if (strcmp(what, "dac") == 0) {
        if (!test_dac((halves != 0u) ? halves : TEST_DAC_HALVES)) { cmd_parser_fail(); }
    } else if (strcmp(what, "all") == 0) {
        console_puts("\r\n[test] ALL - self, clock, sweep, dac\r\n");
        (void)capture_set_clkdiv(ADC_CLKDIV);      /* start slow          */
        const bool self_ok = test_self();
        if (!self_ok) {
            /* The only part whose failure stops the rest: without a
             * working chain every number after it is meaningless. */
            console_puts("[test] ALL: STOPPED - the chain itself does not work\r\n");
            cmd_parser_fail();
            return;
        }
        const bool clock_ok = test_clock();
        (void)test_clkoff();
        /* The bridge between one burst and a thousand, and then every
         * variant with the acceptance test behind it. This is the run
         * that is meant to answer everything at once, because a board
         * run costs a person. */
        test_bursts();
        (void)test_sweep((halves != 0u) ? halves : TEST_SWEEP_HALVES);
        cmd_matrix();
        const bool dac_ok = test_dac(1u);
        console_puts("\r\n[test] ALL DONE\r\n");
        console_puts(self_ok  ? "[test]   self:  PASS\r\n" : "[test]   self:  FAIL\r\n");
        console_puts(clock_ok ? "[test]   clock: PASS\r\n" : "[test]   clock: FAIL\r\n");
        console_puts("[test]   sweep: see the table above\r\n");
        console_puts(dac_ok   ? "[test]   dac:   PASS\r\n" : "[test]   dac:   FAIL\r\n");
        if (!clock_ok || !dac_ok) { cmd_parser_fail(); }
    } else {
        test_list();
        cmd_parser_fail();
    }
}
CMD_DEFINE(test, "test", cmd_test_fn, "test [all|self|clock|clkoff|bursts|matrix|rate|sweep|dac] [n]");

void bench_register_test(void)
{
    (void)cmd_register(&cmd_test);
}

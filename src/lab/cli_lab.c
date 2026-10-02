/*
 * cli_lab.c - the lab's console commands (CORE.3, 02.10.2026): the
 * back-to-back burst mode ("start", "stop", "samc", "input", "core",
 * "clk", "pll"), the instruments ("selftest", "dactest"), the chain test
 * ("chain"), and - through their own registration functions - "sweep"/
 * "test" (bench.c) and "snap"/"rate"/"blk" (b2b_link.c). Moved out of
 * cli.c unchanged; cli.c keeps the console and the core's commands and
 * calls cli_register_lab() (weak there, this one strong) after them.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "adc.h"
#include "timebase.h"
#include "clock.h"
#include "capture.h"
#include "meter.h"     /* the back-to-back instruments (CORE.5) */
#include "adc.h"
#include "dac.h"
#include "dactest.h"
#include "chaintest.h"
#include "bench.h"
#include "led.h"
#include "diag.h"
#include "console.h"
#include "sim.h"
#include "cmd_parser.h"
#include "fmt.h"
#include "stats.h"
#include "uart.h"
#include "gui_link.h"
#include "b2b_link.h"   /* link_register(): snap/rate/blk (CORE.2) */
#include "routing.h"
#include "siggen.h"     /* SG.4: the "siggen" command */
#include "dma_tx.h"     /* SG.4: dma_tx_regs_visit() for "siggen regs" */
#include "sccp.h"       /* SG.4: sccp2_regs_visit() for "siggen regs" */
#include "b2b_link.h"   /* link_register(): snap/rate/blk (CORE.2) */

/* The reply helpers and the DAC-test entry point, defined in cli.c. */
void put_kv(const char *key, uint32_t v);
void put_line(const char *s);
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out);
void usage(const char *text);

static void cmd_start_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    capture_start();
    put_kv("running", 1u);
}
CMD_DEFINE(start, "start", cmd_start_fn, "start - start the burst stream");

static void cmd_stop_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    capture_stop();
    put_kv("running", 0u);
}
CMD_DEFINE(stop, "stop", cmd_stop_fn, "stop - stop after the current buffer");

static void cmd_samc_fn(int argc, char **argv)
{
    uint32_t v;
    if ((argc != 2) || !arg_u32(argv[1], 0u, 31u, &v)) {
        usage("samc <0..31>  (sample time (2*SAMC+0.5) TAD; the rate is set by 'period')");
        return;
    }
    (void)capture_set_input(capture_pinsel(), (uint8_t)v);
    put_kv("samc", v);
}
CMD_DEFINE(samc, "samc", cmd_samc_fn, "samc <0..31> - sample time in TAD steps");

static void cmd_input_fn(int argc, char **argv)
{
    uint32_t v;
    if ((argc != 2) || !arg_u32(argv[1], 0u, 15u, &v)) {
        usage("input <0..15>  (PINSEL of the ADC core, 6 = internal 15/16 VDD)");
        return;
    }
    (void)capture_set_input((uint8_t)v, capture_samc());
    put_kv("input", v);
}
CMD_DEFINE(input, "input", cmd_input_fn, "input <0..15> - analog input (PINSEL)");

static void cmd_core_fn(int argc, char **argv)
{
    uint32_t core, pinsel = ADC_PINSEL;
    if ((argc < 2) || (argc > 3) || !arg_u32(argv[1], 1u, 5u, &core) ||
        ((argc == 3) && !arg_u32(argv[2], 0u, 15u, &pinsel))) {
        usage("core <1..5> [pinsel]  (switch the ADC core; 5 3 = DAC2's pin RA8)");
        return;
    }
    if (!capture_select_core((uint8_t)core, (uint8_t)pinsel, capture_samc())) { cmd_parser_fail(); return; }
    put_kv("core", core);
    put_kv("input", pinsel);
}
CMD_DEFINE(core, "core", cmd_core_fn, "core <1..5> [pinsel] - switch the ADC core");

/* The DAC test measures the DAC inside the chip: UREFCON puts DAC2 on
 * the UREF line and the ADC samples it as AN7, which every core has
 * (board.h). So no core is switched and no pin is involved - run 10 ran
 * the test on core 3 against RA8, which belongs to core 5, and measured
 * an open pin. The input in use is restored afterwards, so a "dactest"
 * from the console does not silently leave the measurement elsewhere. */
/* Not static: src/lab/bench.c's test_dac()/cmd_matrix() call this too
 * (declared extern there) instead of routing DAC2/UREF a second time. */
uint32_t run_dactest(uint32_t bursts)
{
    const uint8_t pinsel_before = capture_pinsel();
    const uint8_t samc_before   = capture_samc();

    if (!uref_route_dac2(false)) {
        put_line("dactest: UREFCON did not take the DAC2 selection");
        return 1u;
    }
    console_kv("[dactest] DAC2 routed to the internal UREF line, INSEL", uref_insel());
    console_kv("[dactest]   measured on this core's AN7, ADC core", adc_core());
    if (!capture_set_input(DAC_UREF_PINSEL, samc_before)) {
        put_line("dactest: could not select the UREF input");
        uref_off();
        return 1u;
    }
    const uint32_t rc = dactest_run(bursts);
    (void)capture_set_input(pinsel_before, samc_before);
    uref_off();
    return rc;
}

static void cmd_dactest_body(int argc, char **argv)
{
    uint32_t halves = 64u;
    if ((argc > 2) || ((argc == 2) && !arg_u32(argv[1], 1u, 10000u, &halves))) {
        usage("dactest [halves]  (capture and judge the DAC2 triangle, default 64 halves)");
        return;
    }
    if (run_dactest(halves) != 0u) { cmd_parser_fail(); }
}
/* Polled transmit while it measures (console_quiet_begin(), cli.c). */
static void cmd_dactest_fn(int argc, char **argv)
{
    const bool quiet = console_quiet_begin();
    cmd_dactest_body(argc, argv);
    console_quiet_end(quiet);
}
CMD_DEFINE(dactest, "dactest", cmd_dactest_fn, "dactest [halves] - judge the running DAC's triangle through the chain");

static void cmd_selftest_body(int argc, char **argv)
{
    uint32_t mean = 0;
    (void)argc; (void)argv;
    const uint32_t rc = capture_selftest(&mean);
    put_kv("selftest_mean", mean);
    if (rc == 0u)      { put_line("selftest: ok (3648..4032)"); }
    else if (rc == 6u) { put_line("selftest: no data - is the stream running?"); }
    else if (rc == 7u) { put_line("selftest: mean outside 3648..4032"); }
    else               { put_line("selftest: DMA channel disabled (address fault?)"); }
    if (rc != 0u) { cmd_parser_fail(); }
}
/* Polled transmit while it measures (console_quiet_begin(), cli.c). */
static void cmd_selftest_fn(int argc, char **argv)
{
    const bool quiet = console_quiet_begin();
    cmd_selftest_body(argc, argv);
    console_quiet_end(quiet);
}
CMD_DEFINE(selftest, "selftest", cmd_selftest_fn, "selftest - sample the internal reference");

static void cmd_clk_fn(int argc, char **argv)
{
    uint32_t v;
    if ((argc != 2) || !arg_u32(argv[1], 100u, 1000u, &v)) {
        usage("clk <100..1000>  (ADC clock divide ratio x 100: 100 = 320 MHz = 40 MSPS, 500 = 64 MHz = 8 MSPS, 1000 = 32 MHz = 4 MSPS, the slowest the ADC may run)");
        return;
    }
    const uint32_t rc = capture_set_clkdiv(v);
    put_kv("ratio asked for", v);
    put_kv("ratio read back", capture_clkdiv());
    put_kv("adc clock Hz", clock_adc_hz());
    put_kv("ksps nominal", capture_nominal_ksps(v));
    put_line(clock_adc_div_error(rc));
    if (rc != CLKDIV_OK) { cmd_parser_fail(); }
}
CMD_DEFINE(clk, "clk", cmd_clk_fn, "clk <100..1000> - CLKGEN6 divide ratio x 100 (does NOT change the rate)");

static void cmd_pll_fn(int argc, char **argv)
{
    uint32_t p1, p2;
    if ((argc != 3) || !arg_u32(argv[1], 1u, 7u, &p1) || !arg_u32(argv[2], 1u, 7u, &p2)) {
        usage("pll <postdiv1 1..7> <postdiv2 1..7>  (ADC clock = 1600 MHz / (p1*p2), p1 >= p2; 5 1 = 320 MHz = 40 MSPS, 5 5 = 64 MHz = 8 MSPS, 7 7 = 32.65 MHz = 4.08 MSPS)");
        return;
    }
    const uint32_t rc = capture_set_pll(p1, p2);
    put_kv("postdiv1", clock_adc_pll_postdiv1());
    put_kv("postdiv2", clock_adc_pll_postdiv2());
    put_kv("adc clock Hz", clock_adc_hz());
    put_kv("ksps nominal", capture_nominal_ksps(0u));
    put_line(clock_adc_div_error(rc));
    if (rc != CLKDIV_OK) { cmd_parser_fail(); }
}
CMD_DEFINE(pll, "pll", cmd_pll_fn, "pll <p1> <p2> - PLL1 output dividers = the sample rate");

/* The chain test (chaintest.c). "chain all" is the one command the
 * person at the board types; the rest is for repeating a part. */
static void cmd_chain_body(int argc, char **argv)
{
    static const char use[] = "chain all | chain <0..9> | chain from <0..9> | chain run <ksps> [seconds]";
    uint32_t a = 0u, b = 0u;
    if ((argc == 2) && (strcmp(argv[1], "all") == 0)) {
        chain_all(0u, 9u);
    } else if ((argc == 3) && (strcmp(argv[1], "from") == 0) && arg_u32(argv[2], 0u, 9u, &a)) {
        chain_all(a, 9u);
    } else if ((argc == 2) && arg_u32(argv[1], 0u, 9u, &a)) {
        chain_all(a, a);
    } else if (((argc == 3) || (argc == 4)) && (strcmp(argv[1], "run") == 0) &&
               arg_u32(argv[2], 1u, 40000u, &a) &&
               ((argc == 3) || arg_u32(argv[3], 1u, 3600u, &b))) {
        chain_run(a, (argc == 4) ? b : 10u);
    } else {
        usage(use);
    }
}
/* Polled transmit while it measures (console_quiet_begin(), cli.c). */
static void cmd_chain_fn(int argc, char **argv)
{
    const bool quiet = console_quiet_begin();
    cmd_chain_body(argc, argv);
    console_quiet_end(quiet);
}
CMD_DEFINE(chain, "chain", cmd_chain_fn, "chain all|<n>|from <n>|run <ksps> [s] - the chain test");

void cli_register_lab(void)
{
    (void)cmd_register(&cmd_start);
    (void)cmd_register(&cmd_stop);
    (void)cmd_register(&cmd_samc);
    (void)cmd_register(&cmd_input);
    (void)cmd_register(&cmd_selftest);
    bench_register_sweep();
    (void)cmd_register(&cmd_clk);
    (void)cmd_register(&cmd_pll);
    bench_register_test();
    (void)cmd_register(&cmd_core);
    (void)cmd_register(&cmd_dactest);
    link_register();
    (void)cmd_register(&cmd_chain);
}

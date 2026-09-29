/*
 * test_siggen.c - host-side test for src/siggen/siggen.c (SG.3, 29.09.2026)
 *
 * Built by tools\hosttest.bat against the real siggen.c and lib/wavegen.c;
 * every driver it calls (dma.c channel 1, sccp.c SCCP2, dac.c, routing.c's
 * generator claim, timebase.c) is stubbed here with a call log - the
 * test_routing.c pattern. What it checks:
 *   - start calls the drivers in the order siggen.h promises (DAC level,
 *     DMA channel 1 on the DAC's data half with 16-bit transfers and the
 *     SCCP2 trigger, SCCP2 last) and stop in the reverse order;
 *   - every refusal - bad dac/n/rate, lo/hi outside the DAC's range
 *     without force, each wavegen error code, a routing refusal, a driver
 *     that refuses - happens with nothing left running, and the checks
 *     come before the first driver call;
 *   - snap: the reported f0 is a whole number of periods in the table, at
 *     the rate SCCP2 really runs (its period in whole input clocks);
 *   - siggen_set()'s parameter names and ranges, and that the table the
 *     DMA plays is wavegen's for the configured parameters;
 *   - siggen_release_dac() stops only the generator on that DAC.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "siggen.h"
#include "dma_tx.h"
#include "sccp.h"
#include "dac.h"
#include "routing.h"
#include "timebase.h"
#include "wavegen.h"
#include "check.h"

/* ---- the call log ---- */
static char log_[512];
static void logc(const char *s) { strncat(log_, s, sizeof log_ - strlen(log_) - 1u); }
static void log_clear(void) { log_[0] = '\0'; }

/* ---- stubs ---- */
static bool     dma_ok = true, sccp_ok = true, dac_ok = true;
static route_err_t route_rc = ROUTE_OK;
static uint32_t dma_trig, dma_n, dma_size;
static const volatile void *dma_src;
static volatile void *dma_dst;
static uint32_t sccp_ticks;
static uint8_t  dac_unit;
static uint16_t dac_code;
static uint32_t dac_regs[2];              /* stand-ins for DAC1DAT/DAC2DAT */

bool dma1_tx_start(uint32_t trigger, const volatile void *src, uint32_t n,
                   volatile void *dst_sfr, uint32_t size)
{
    logc("dma+");
    dma_trig = trigger; dma_src = src; dma_n = n; dma_dst = dst_sfr; dma_size = size;
    return dma_ok;
}
void     dma1_tx_stop(void)      { logc("dma-"); }
uint32_t dma1_tx_status(void)    { return 0u; }
uint32_t dma1_tx_remaining(void) { return dma_n; }
bool     dma1_tx_enabled(void)   { return true; }
uint32_t dma_window_gap(void)    { return 0u; }

bool sccp2_start(uint32_t ticks, sccp2_pace_t pace)
{
    (void)pace;
    logc("sccp+");
    sccp_ticks = ticks;
    return sccp_ok;
}
void     sccp2_stop(void)        { logc("sccp-"); }
uint32_t sccp2_hz(void)          { return 100000000u; }
uint32_t sccp2_actual_hz(void)   { return sccp_ticks ? (100000000u + sccp_ticks / 2u) / sccp_ticks : 0u; }
void     sccp2_flags_clear(void) { }
uint32_t sccp2_flags_read(void)  { return 0u; }

bool dac_level_start(uint8_t unit, uint16_t code)
{
    logc("dac+");
    dac_unit = unit; dac_code = code;
    return dac_ok;
}
void dac_off(uint8_t unit) { (void)unit; logc("dac-"); }
volatile void *dac_dma_target(uint8_t unit)
{
    return (volatile uint8_t *)&dac_regs[unit - 1u] + 2;
}

route_err_t routing_gen_check(uint8_t dac, uint32_t n) { (void)dac; (void)n; logc("chk"); return route_rc; }
route_err_t routing_gen_claim(uint8_t dac, uint32_t n) { (void)dac; (void)n; logc("claim"); return route_rc; }
void        routing_gen_release(void)                  { logc("rel"); }

/* The measurement in siggen_visit() spins on this: a counter that moves. */
static uint32_t tb = 0u;
uint32_t timebase_ticks(void) { tb += 1000u; return tb; }

static void visit_nop(const char *name, int64_t v, siggen_vis_fmt_t fmt) { (void)name; (void)v; (void)fmt; }

static void set_defaults(void)
{
    CHECK_EQ(siggen_set("f0", 10000LL * 1000000), SIGGEN_OK);
    CHECK_EQ(siggen_set("h2", 200000), SIGGEN_OK);
    CHECK_EQ(siggen_set("h3", 400000), SIGGEN_OK);
    CHECK_EQ(siggen_set("h4", 100000), SIGGEN_OK);
    CHECK_EQ(siggen_set("h5", 0), SIGGEN_OK);
    CHECK_EQ(siggen_set("h6", 0), SIGGEN_OK);
    CHECK_EQ(siggen_set("h7", 0), SIGGEN_OK);
    CHECK_EQ(siggen_set("decay", 0), SIGGEN_OK);
    CHECK_EQ(siggen_set("amp", 1000000), SIGGEN_OK);
    CHECK_EQ(siggen_set("lo", 205LL * 1000000), SIGGEN_OK);
    CHECK_EQ(siggen_set("hi", 3890LL * 1000000), SIGGEN_OK);
}

/* A refusal: the code, and no driver called at all (the checks first). */
static void refused_before_drivers(siggen_result_t want, uint8_t dac, uint32_t n,
                                   uint32_t hz, bool force)
{
    log_clear();
    const siggen_result_t r = siggen_start(dac, n, hz, true, force, 0u);
    CHECK_EQ(r, want);
    CHECK(!siggen_running());
    CHECK(strstr(log_, "dac+") == NULL && strstr(log_, "dma+") == NULL &&
          strstr(log_, "sccp+") == NULL);
    if (r != want) { fprintf(stderr, "  got %d (%s), want %d\n", r, siggen_result_name(r), want); }
}

int main(void)
{
    set_defaults();

    /* ---- siggen_set(): names and ranges ---- */
    CHECK_EQ(siggen_set("f1", 1), SIGGEN_E_PARAM);
    CHECK_EQ(siggen_set("h1", 1), SIGGEN_E_PARAM);
    CHECK_EQ(siggen_set("h8", 1), SIGGEN_E_PARAM);
    CHECK_EQ(siggen_set("h", 1), SIGGEN_E_PARAM);
    CHECK_EQ(siggen_set("f0", 0), SIGGEN_E_VALUE);
    CHECK_EQ(siggen_set("f0", -5), SIGGEN_E_VALUE);
    CHECK_EQ(siggen_set("amp", 0), SIGGEN_E_VALUE);
    CHECK_EQ(siggen_set("amp", 1000001), SIGGEN_E_VALUE);
    CHECK_EQ(siggen_set("decay", -1), SIGGEN_E_VALUE);
    CHECK_EQ(siggen_set("h7", -100000001), SIGGEN_E_VALUE);
    CHECK_EQ(siggen_set("lo", 1500000), SIGGEN_E_VALUE);          /* 1.5: not a code */
    CHECK_EQ(siggen_set("hi", 4096LL * 1000000), SIGGEN_E_VALUE);

    /* ---- a clean start: order, arguments, table ---- */
    log_clear();
    siggen_result_t r = siggen_start(2u, 1000u, 100000u, true, false, 0u);
    CHECK_EQ(r, SIGGEN_OK);
    CHECK(siggen_running());
    CHECK_EQ(siggen_dac(), 2u);
    CHECK(strcmp(log_, "chkdac+dma+sccp+claim") == 0);
    if (strcmp(log_, "chkdac+dma+sccp+claim") != 0) { fprintf(stderr, "  order: %s\n", log_); }
    CHECK_EQ(dma_trig, DMA_TRIG_SCCP2);
    CHECK_EQ(dma_size, DMA_SIZE_16);
    CHECK_EQ(dma_n, 1000u);
    CHECK(dma_src == siggen_table());
    CHECK(dma_dst == (volatile uint8_t *)&dac_regs[1] + 2);        /* DAC2DAT's upper half */
    CHECK_EQ(sccp_ticks, 1000u);                                    /* 100 MHz / 100 kHz */
    CHECK_EQ(dac_unit, 2u);
    CHECK_EQ(dac_code, siggen_table()[0]);

    /* The table the DMA plays is wavegen's, for the same parameters. */
    {
        static uint16_t ref[1000];
        wavegen_cfg_t c = { .n = 1000u, .play_hz = 100000u, .f0_hz = 10000.0f,
                            .harm = { 0.2f, 0.4f, 0.1f, 0.0f, 0.0f, 0.0f },
                            .decay = 0.0f, .amplitude = 1.0f, .out_min = 205u, .out_max = 3890u };
        float f0u = 0.0f;
        CHECK_EQ(wavegen_fill(&c, ref, true, &f0u), WAVEGEN_OK);
        CHECK(memcmp(ref, siggen_table(), sizeof ref) == 0);
    }

    /* snap: 10 kHz at 100 kHz over 1000 entries is exactly 100 periods */
    CHECK(fabsf(siggen_f0_used() - 10000.0f) < 0.01f);
    /* ... and at a rate SCCP2 cannot hit exactly (100 MHz / 30 kHz =
     * 3333.3 -> 3333 clocks = 30003.0 Hz) f0 is snapped at the REAL rate */
    CHECK_EQ(siggen_set("f0", 1234LL * 1000000), SIGGEN_OK);
    r = siggen_start(1u, 500u, 30000u, true, false, 0u);
    CHECK_EQ(r, SIGGEN_OK);
    {
        const float real = 100000000.0f / 3333.0f;
        const float periods = siggen_f0_used() * 500.0f / real;
        CHECK(fabsf(periods - roundf(periods)) < 1e-3f);
        CHECK(fabsf(periods - 21.0f) < 1e-3f);                  /* round(1234 x 500 / 30003) */
    }
    CHECK_EQ(siggen_set("f0", 10000LL * 1000000), SIGGEN_OK);

    /* status runs without a console */
    siggen_visit(visit_nop);

    /* ---- stop: reverse order, claim released ---- */
    log_clear();
    siggen_stop();
    CHECK(strcmp(log_, "sccp-dma-dac-rel") == 0);
    if (strcmp(log_, "sccp-dma-dac-rel") != 0) { fprintf(stderr, "  stop order: %s\n", log_); }
    CHECK(!siggen_running());
    CHECK_EQ(siggen_dac(), 0u);
    log_clear();
    siggen_stop();                                   /* a second stop does nothing */
    CHECK(log_[0] == '\0');

    /* ---- refusals before any driver call ---- */
    refused_before_drivers(SIGGEN_E_DAC, 0u, 1000u, 100000u, false);
    refused_before_drivers(SIGGEN_E_DAC, 3u, 1000u, 100000u, false);
    refused_before_drivers(SIGGEN_E_N, 2u, 1u, 100000u, false);
    refused_before_drivers(SIGGEN_E_N, 2u, SIGGEN_N_MAX + 1u, 100000u, false);
    refused_before_drivers(SIGGEN_E_RATE, 2u, 1000u, SIGGEN_PLAY_HZ_MIN - 1u, false);
    refused_before_drivers(SIGGEN_E_RATE, 2u, 1000u, SIGGEN_PLAY_HZ_MAX + 1u, false);
    /* lo/hi outside 205..3890: refused without force, taken with it */
    CHECK_EQ(siggen_set("lo", 100LL * 1000000), SIGGEN_OK);
    refused_before_drivers(SIGGEN_E_RANGE, 2u, 1000u, 100000u, false);
    CHECK_EQ(siggen_start(2u, 1000u, 100000u, true, true, 0u), SIGGEN_OK);
    CHECK(dac_code >= 205u && dac_code <= 3890u);  /* the static level stays in range */
    siggen_stop();
    CHECK_EQ(siggen_set("lo", 205LL * 1000000), SIGGEN_OK);
    /* every wavegen error code comes back as SIGGEN_E_WAVEGEN, with the code */
    CHECK_EQ(siggen_set("f0", 60000LL * 1000000), SIGGEN_OK);      /* >= play_hz / 2 */
    refused_before_drivers(SIGGEN_E_WAVEGEN, 2u, 1000u, 100000u, false);
    CHECK_EQ(siggen_wavegen_err(), WAVEGEN_E_F0);
    CHECK_EQ(siggen_set("f0", 10000LL * 1000000), SIGGEN_OK);
    CHECK_EQ(siggen_set("hi", 205LL * 1000000), SIGGEN_OK);        /* lo >= hi */
    refused_before_drivers(SIGGEN_E_WAVEGEN, 2u, 1000u, 100000u, false);
    CHECK_EQ(siggen_wavegen_err(), WAVEGEN_E_RANGE);
    CHECK_EQ(siggen_set("hi", 3890LL * 1000000), SIGGEN_OK);
    CHECK_EQ(siggen_set("decay", 100000000000000LL), SIGGEN_OK);   /* 1e8 /s: exp() underflows to 0 after sample 0 */
    refused_before_drivers(SIGGEN_E_WAVEGEN, 2u, 1000u, 100000u, false);
    CHECK_EQ(siggen_wavegen_err(), WAVEGEN_E_FLAT);
    CHECK_EQ(siggen_set("decay", 0), SIGGEN_OK);
    /* routing refuses (DAC2 is the test stream's triangle) */
    route_rc = ROUTE_ERR_DAC_BUSY;
    refused_before_drivers(SIGGEN_E_ROUTE, 2u, 1000u, 100000u, false);
    CHECK_EQ(siggen_route_err(), ROUTE_ERR_DAC_BUSY);
    route_rc = ROUTE_OK;

    /* ---- a driver refusing: nothing left running ---- */
    dac_ok = false;
    CHECK_EQ(siggen_start(2u, 1000u, 100000u, true, false, 0u), SIGGEN_E_DAC_START);
    CHECK(!siggen_running());
    dac_ok = true;
    dma_ok = false;
    log_clear();
    CHECK_EQ(siggen_start(2u, 1000u, 100000u, true, false, 0u), SIGGEN_E_DMA);
    CHECK(!siggen_running());
    CHECK(strstr(log_, "dac-") != NULL);           /* the DAC taken down again */
    dma_ok = true;
    sccp_ok = false;
    log_clear();
    CHECK_EQ(siggen_start(2u, 1000u, 100000u, true, false, 0u), SIGGEN_E_CLOCK);
    CHECK(!siggen_running());
    CHECK(strstr(log_, "dma-") != NULL && strstr(log_, "dac-") != NULL);
    CHECK(strstr(log_, "claim") == NULL);          /* nothing claimed */
    sccp_ok = true;

    /* ---- siggen_release_dac(): only the generator's own DAC ---- */
    CHECK_EQ(siggen_start(1u, 1000u, 100000u, true, false, 0u), SIGGEN_OK);
    siggen_release_dac(2u);
    CHECK(siggen_running());
    siggen_release_dac(1u);
    CHECK(!siggen_running());

    return check_summary();
}

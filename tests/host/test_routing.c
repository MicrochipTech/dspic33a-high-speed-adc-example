/*
 * test_routing.c - host-side test for src/app/routing.c (see routing.h),
 * one function per conflict/resource rule (docs/IMPLEMENTATION-PLAN.md
 * P11.2), each with a case that just passes and a case that triggers the
 * rule's own route_err_t. Every test calls routing_clear() first, so the
 * tests do not depend on each other's order.
 *
 * Routes are built with designated initializers, leaving `dac`/
 * `table_samples` at 0 unless the rule under test needs them - see
 * routing.h's route_t comment for what each field means.
 *
 * Where a rule needs several routes to reach its limit (DMA, SCCP: both are
 * 8), the routes are built from ROUTE_SRC_RAM_TABLE with table_samples > 0
 * ("a route that plays a table"), because that is the one route shape whose
 * resource cost is not also bounded by ROUTE_ADC_CORES (5) - see routing.c's
 * top comment for the DMA/SCCP check order this relies on.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "routing.h"
#include "acquisition.h"
#include "check.h"

/* ---- P11.3: the two acquisition.c functions routing_apply() calls, as
 * counting stubs (acquisition.c itself needs the device: xc.h, the drivers).
 * What matters to the tests below is WHETHER and with WHAT they were called,
 * and that a refused route never reaches them. ---- */
static int      stub_setup_calls, stub_restore_calls;
static uint8_t  stub_core, stub_pinsel, stub_samc;
static bool     stub_test_dac;
static bool     stub_setup_result = true;

bool acq_chain_setup_input(uint8_t core, uint8_t pinsel, uint8_t samc, bool test_dac)
{
    stub_setup_calls++;
    stub_core = core; stub_pinsel = pinsel; stub_samc = samc; stub_test_dac = test_dac;
    return stub_setup_result;
}

void acq_chain_restore(void)
{
    stub_restore_calls++;
}

static void stubs_reset(bool setup_result)
{
    stub_setup_calls = 0; stub_restore_calls = 0;
    stub_core = 0u; stub_pinsel = 0u; stub_samc = 0u; stub_test_dac = false;
    stub_setup_result = setup_result;
}

static void test_route_pin_reachable(void)
{
    /* core 1 and core 5 reach PINSEL 0..4 only (tools/gen_route_pins.py's
     * route_pin_mask, 0x1F); core 2, 3, 4 reach 0..5 (0x3F). */
    CHECK(route_pin_reachable(1u, 0u));
    CHECK(route_pin_reachable(1u, 4u));
    CHECK(!route_pin_reachable(1u, 5u));
    CHECK(route_pin_reachable(2u, 5u));

    /* PINSEL 6 (the 15/16 VDD reference) and 7 (UREF) are internal
     * channels of every core - DS70005591D Table 16-2, the ATDF's
     * "ADnAN6"/"ADnAN7" params (P11.4: until then 6 was refused, which
     * would have broken the GUI's "6 = internal ref"). */
    for (uint32_t core = 1u; core <= ROUTE_ADC_CORES; core++) {
        CHECK(route_pin_reachable((uint8_t)core, ROUTE_PINSEL_VREF));
        CHECK(route_pin_reachable((uint8_t)core, ROUTE_PINSEL_UREF));
    }

    /* core 5's two further internal channels (ATDF: AD5AN5 "Touch ADC
     * Input", AD5AN8 "VDDCORE"); no other core has them */
    CHECK(route_pin_reachable(5u, 5u));
    CHECK(route_pin_reachable(5u, 8u));
    CHECK(!route_pin_reachable(1u, 8u));
    CHECK(!route_pin_reachable(4u, 8u));

    /* PINSEL 9..15 name nothing in the ATDF: refused on every core */
    for (uint32_t core = 1u; core <= ROUTE_ADC_CORES; core++) {
        for (uint32_t p = 9u; p <= 15u; p++) {
            CHECK(!route_pin_reachable((uint8_t)core, (uint8_t)p));
        }
    }
    CHECK(!route_pin_reachable(1u, 16u));

    /* no core 0, no core past ROUTE_ADC_CORES. */
    CHECK(!route_pin_reachable(0u, 0u));
    CHECK(!route_pin_reachable((uint8_t)(ROUTE_ADC_CORES + 1u), 0u));
}

static void test_pin_reachability(void)
{
    routing_clear();
    route_t ok = { .src = ROUTE_SRC_EXT, .core = 1u, .pinsel = 0u,
                   .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&ok), ROUTE_OK);

    routing_clear();
    route_t bad = { .src = ROUTE_SRC_EXT, .core = 1u, .pinsel = 5u,
                    .sink = ROUTE_SINK_STREAM }; /* core 1 stops at PINSEL 4 */
    CHECK_EQ(routing_add(&bad), ROUTE_ERR_PIN_UNREACHABLE);
}

static void test_core_exclusivity(void)
{
    routing_clear();
    route_t a = { .src = ROUTE_SRC_EXT, .core = 1u, .pinsel = 0u,
                  .sink = ROUTE_SINK_STREAM };
    route_t b = { .src = ROUTE_SRC_EXT, .core = 2u, .pinsel = 0u,
                  .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&a), ROUTE_OK);
    CHECK_EQ(routing_add(&b), ROUTE_OK);         /* distinct cores: both fit */

    route_t c = { .src = ROUTE_SRC_EXT, .core = 1u, .pinsel = 1u,
                  .sink = ROUTE_SINK_STREAM };    /* core 1 again            */
    CHECK_EQ(routing_add(&c), ROUTE_ERR_CORE_IN_USE);
}

static void test_dma_limit(void)
{
    routing_clear();
    /* one route per ADC core - all 5 that exist, 5 of the 8 DMA channels */
    for (uint32_t core = 1u; core <= ROUTE_ADC_CORES; core++) {
        route_t r = { .src = ROUTE_SRC_EXT, .core = (uint8_t)core,
                      .pinsel = 0u, .sink = ROUTE_SINK_STREAM };
        CHECK_EQ(routing_add(&r), ROUTE_OK);
    }
    /* 3 more routes that only play a table (no ADC core involved) take the
     * count from 5 to 8 - still within the limit */
    for (uint32_t i = 0u; i < 3u; i++) {
        route_t r = { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE,
                      .sink = ROUTE_SINK_STREAM, .dac = (uint8_t)(i + 1u),
                      .table_samples = 50u };
        CHECK_EQ(routing_add(&r), ROUTE_OK);
    }
    /* the 9th DMA-consuming route (5 ADC + 4 table) exceeds the 8 channels */
    route_t over = { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE,
                     .sink = ROUTE_SINK_STREAM, .dac = 4u,
                     .table_samples = 50u };
    CHECK_EQ(routing_add(&over), ROUTE_ERR_DMA_LIMIT);
}

static void test_sccp_limit(void)
{
    routing_clear();
    /* 8 table-playing routes, no ADC core at all: each costs one playback
     * SCCP, so this alone reaches the 8-SCCP limit without needing the
     * shared ADC-trigger slot */
    for (uint32_t i = 0u; i < 8u; i++) {
        route_t r = { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE,
                      .sink = ROUTE_SINK_STREAM, .dac = (uint8_t)(i % 8u + 1u),
                      .table_samples = 50u };
        CHECK_EQ(routing_add(&r), ROUTE_OK);
    }
    /* the 9th would need a 9th SCCP (and, equally, a 9th DMA channel - SCCP
     * is checked first, see routing.c's top comment, so this is what proves
     * the SCCP rule specifically) */
    route_t over = { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE,
                     .sink = ROUTE_SINK_STREAM, .dac = 1u,
                     .table_samples = 50u };
    CHECK_EQ(routing_add(&over), ROUTE_ERR_SCCP_LIMIT);
}

static void test_dac_outputs(void)
{
    routing_clear();
    route_t a = { .src = ROUTE_SRC_DAC_PIN, .core = 1u, .pinsel = 1u,
                  .dac = 1u, .sink = ROUTE_SINK_STREAM };
    route_t b = { .src = ROUTE_SRC_DAC_PIN, .core = 2u, .pinsel = 1u,
                  .dac = 2u, .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&a), ROUTE_OK);
    CHECK_EQ(routing_add(&b), ROUTE_OK);        /* both DACOUT1/2 in use    */

    route_t c = { .src = ROUTE_SRC_DAC_PIN, .core = 3u, .pinsel = 1u,
                  .dac = 3u, .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&c), ROUTE_ERR_DAC_OUTPUTS);
}

static void test_uref(void)
{
    routing_clear();
    route_t a = { .src = ROUTE_SRC_DAC_INT, .core = 1u,
                  .pinsel = ROUTE_PINSEL_UREF, .dac = 1u,
                  .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&a), ROUTE_OK);

    route_t b = { .src = ROUTE_SRC_DAC_INT, .core = 2u,
                  .pinsel = ROUTE_PINSEL_UREF, .dac = 2u,
                  .sink = ROUTE_SINK_STREAM };    /* a second DAC on UREF   */
    CHECK_EQ(routing_add(&b), ROUTE_ERR_UREF_BUSY);
}

static void test_ram_budget(void)
{
    const uint32_t fits = ROUTE_RAM_BUDGET_BYTES / ROUTE_SAMPLE_BYTES;

    routing_clear();
    route_t ok = { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE,
                   .sink = ROUTE_SINK_STREAM, .dac = 1u,
                   .table_samples = fits };       /* exactly the budget     */
    CHECK_EQ(routing_add(&ok), ROUTE_OK);

    routing_clear();
    route_t over = { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE,
                     .sink = ROUTE_SINK_STREAM, .dac = 1u,
                     .table_samples = fits + 1u }; /* one sample over        */
    CHECK_EQ(routing_add(&over), ROUTE_ERR_RAM_BUDGET);
}

static void test_not_yet(void)
{
    routing_clear();
    route_t ram_sink = { .src = ROUTE_SRC_EXT, .core = 3u, .pinsel = 5u,
                         .sink = ROUTE_SINK_RAM }; /* fits every resource
                                                    * rule, wrong sink       */
    CHECK_EQ(routing_add(&ram_sink), ROUTE_ERR_NOT_YET);

    routing_clear();
    route_t console_sink = { .src = ROUTE_SRC_EXT, .core = 3u, .pinsel = 5u,
                             .sink = ROUTE_SINK_CONSOLE };
    CHECK_EQ(routing_add(&console_sink), ROUTE_ERR_NOT_YET);

    routing_clear();
    route_t stream_sink = { .src = ROUTE_SRC_EXT, .core = 3u, .pinsel = 5u,
                            .sink = ROUTE_SINK_STREAM }; /* same route,
                                                          * the wired sink   */
    CHECK_EQ(routing_add(&stream_sink), ROUTE_OK);
}

/* ---- P11.3: routing_apply() ---- */

/* The stream route as acquisition.c defines ROUTE_STREAM for the EV74H48A
 * (that definition lives in acquisition.c, which does not build on the
 * host - the trace scenario tests/trace/scenarios/route_stream.c applies
 * the real one). */
static const route_t stream_like = {
    .src = ROUTE_SRC_DAC_PIN, .core = 5u, .pinsel = 3u, .dac = 2u,
    .sink = ROUTE_SINK_STREAM, .table_samples = 0u, .samc = 0u,
};

/* SG.5 (29.09.2026): the signal generator's claim. DAC2 busy in both
 * directions (the test stream's triangle vs. the generator), its DAC
 * output/DMA/SCCP/RAM counted against the limits, and routing_clear()
 * ("stream off") leaving the claim alone. */
static void test_generator_claim(void)
{
    routing_clear();
    routing_gen_release();
    stubs_reset(true);

    /* generator on DAC2 first: the test stream (DAC2 triangle) is refused
     * before any driver call, an external input on RA8 (the loop) is not */
    CHECK_EQ(routing_gen_claim(2u, 1000u), ROUTE_OK);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_ERR_DAC_BUSY);
    CHECK_EQ(stub_setup_calls, 0);
    route_t loop = { .src = ROUTE_SRC_EXT, .core = 5u, .pinsel = 3u,
                     .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_apply(&loop), ROUTE_OK);
    /* a DAC_PIN route on DAC1 still fits one output ... */
    route_t d1 = { .src = ROUTE_SRC_DAC_PIN, .core = 1u, .pinsel = 1u,
                   .dac = 1u, .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&d1), ROUTE_OK);
    /* ... and then both outputs are taken (generator + DAC1) */
    route_t d3 = { .src = ROUTE_SRC_DAC_PIN, .core = 2u, .pinsel = 1u,
                   .dac = 3u, .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&d3), ROUTE_ERR_DAC_OUTPUTS);
    /* "stream off" clears the routes, not the claim */
    routing_clear();
    CHECK_EQ(routing_apply(&stream_like), ROUTE_ERR_DAC_BUSY);
    routing_gen_release();
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);

    /* the reverse: the test stream runs, the generator may not take DAC2
     * but may take DAC1 */
    CHECK_EQ(routing_gen_check(2u, 1000u), ROUTE_ERR_DAC_BUSY);
    CHECK_EQ(routing_gen_claim(2u, 1000u), ROUTE_ERR_DAC_BUSY);
    CHECK_EQ(routing_gen_claim(1u, 1000u), ROUTE_OK);
    routing_gen_release();

    /* its table counts against the RAM budget */
    routing_clear();
    CHECK_EQ(routing_gen_check(1u, ROUTE_RAM_BUDGET_BYTES / 2u), ROUTE_OK);
    CHECK_EQ(routing_gen_check(1u, ROUTE_RAM_BUDGET_BYTES / 2u + 1u), ROUTE_ERR_RAM_BUDGET);
    routing_gen_release();
}

static void test_apply_stream(void)
{
    routing_clear();
    stubs_reset(true);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);
    CHECK_EQ(stub_setup_calls, 1);         /* the one setup call ...        */
    CHECK_EQ(stub_restore_calls, 0);       /* ... and no restore            */
    CHECK_EQ(stub_core, 5u);               /* with the route's input ...    */
    CHECK_EQ(stub_pinsel, 3u);
    CHECK_EQ(stub_samc, 0u);
    CHECK(stub_test_dac);                  /* ... and DAC2 as the signal    */

    /* the route is recorded: core 5 is now taken */
    route_t again = { .src = ROUTE_SRC_EXT, .core = 5u, .pinsel = 0u,
                      .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&again), ROUTE_ERR_CORE_IN_USE);
}

static void test_apply_ext(void)
{
    /* "stream on <ksps> <core> <pinsel> <samc>": any reachable pin, the DAC
     * left alone, SAMC passed through */
    routing_clear();
    stubs_reset(true);
    route_t ext = { .src = ROUTE_SRC_EXT, .core = 2u, .pinsel = ROUTE_PINSEL_UREF,
                    .sink = ROUTE_SINK_STREAM, .samc = 1u };
    CHECK_EQ(routing_apply(&ext), ROUTE_OK);
    CHECK_EQ(stub_setup_calls, 1);
    CHECK_EQ(stub_core, 2u);
    CHECK_EQ(stub_pinsel, ROUTE_PINSEL_UREF);
    CHECK_EQ(stub_samc, 1u);
    CHECK(!stub_test_dac);
}

static void test_apply_precheck_refuses_before_any_call(void)
{
    /* a conflicting route: core 5 already in use (added, not applied) */
    routing_clear();
    stubs_reset(true);
    route_t holder = { .src = ROUTE_SRC_EXT, .core = 5u, .pinsel = 0u,
                       .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_add(&holder), ROUTE_OK);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_ERR_CORE_IN_USE);
    CHECK_EQ(stub_setup_calls, 0);
    CHECK_EQ(stub_restore_calls, 0);

    /* an unreachable pin: the first rule, same result */
    routing_clear();
    stubs_reset(true);
    route_t bad_pin = { .src = ROUTE_SRC_EXT, .core = 1u, .pinsel = 5u,
                        .sink = ROUTE_SINK_STREAM };
    CHECK_EQ(routing_apply(&bad_pin), ROUTE_ERR_PIN_UNREACHABLE);
    CHECK_EQ(stub_setup_calls, 0);

    /* a sink that is not wired up: routing_add()'s own NOT_YET */
    routing_clear();
    stubs_reset(true);
    route_t ram_sink = stream_like;
    ram_sink.sink = ROUTE_SINK_RAM;
    CHECK_EQ(routing_apply(&ram_sink), ROUTE_ERR_NOT_YET);
    CHECK_EQ(stub_setup_calls, 0);
}

static void test_apply_not_yet_shapes(void)
{
    /* every shape acquisition.c cannot run today: NOT_YET, no driver call,
     * nothing recorded (the same core applies fine afterwards) */
    const route_t shapes[] = {
        { .src = ROUTE_SRC_DAC_INT, .core = 5u, .pinsel = ROUTE_PINSEL_UREF,
          .dac = 2u, .sink = ROUTE_SINK_STREAM },            /* UREF route     */
        { .src = ROUTE_SRC_DAC_PIN, .core = 5u, .pinsel = 3u, .dac = 1u,
          .sink = ROUTE_SINK_STREAM },                       /* not DAC2       */
        { .src = ROUTE_SRC_DAC_PIN, .core = 5u, .pinsel = 3u, .dac = 2u,
          .sink = ROUTE_SINK_STREAM, .table_samples = 256u }, /* plays a table  */
        { .src = ROUTE_SRC_EXT, .core = 5u, .pinsel = 3u,
          .sink = ROUTE_SINK_STREAM, .table_samples = 256u }, /* plays a table  */
        { .src = ROUTE_SRC_RAM_TABLE, .core = ROUTE_CORE_NONE, .dac = 1u,
          .sink = ROUTE_SINK_STREAM, .table_samples = 256u }, /* no ADC at all  */
    };
    for (uint32_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        routing_clear();
        stubs_reset(true);
        CHECK_EQ(routing_apply(&shapes[i]), ROUTE_ERR_NOT_YET);
        CHECK_EQ(stub_setup_calls, 0);
        CHECK_EQ(stub_restore_calls, 0);
        CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);  /* nothing recorded */
    }
}

static void test_internal_channels_pass_the_rules(void)
{
    /* route_check() (through routing_add()) accepts every core on PINSEL 6
     * and 7 - "stream on <ksps> <core> 6|7", the GUI's internal inputs -
     * and refuses a pin the core does not bring out (P11.2's rule, the one
     * refusal P11.4 added to "stream on"). Distinct cores, so the second
     * add of the same core is what is under test, not core exclusivity. */
    for (uint32_t core = 1u; core <= ROUTE_ADC_CORES; core++) {
        routing_clear();
        route_t vref = { .src = ROUTE_SRC_EXT, .core = (uint8_t)core,
                         .pinsel = ROUTE_PINSEL_VREF, .sink = ROUTE_SINK_STREAM };
        CHECK_EQ(routing_add(&vref), ROUTE_OK);
        routing_clear();
        route_t uref = { .src = ROUTE_SRC_EXT, .core = (uint8_t)core,
                         .pinsel = ROUTE_PINSEL_UREF, .sink = ROUTE_SINK_STREAM };
        CHECK_EQ(routing_add(&uref), ROUTE_OK);
    }
    routing_clear();
    route_t unbonded = { .src = ROUTE_SRC_EXT, .core = 5u, .pinsel = 9u,
                         .sink = ROUTE_SINK_STREAM };     /* AD5AN9: no pin, no name */
    CHECK_EQ(routing_add(&unbonded), ROUTE_ERR_PIN_UNREACHABLE);
    route_t unbonded2 = { .src = ROUTE_SRC_EXT, .core = 1u, .pinsel = 5u,
                          .sink = ROUTE_SINK_STREAM };    /* AD1AN5: not on the MPS512 */
    CHECK_EQ(routing_add(&unbonded2), ROUTE_ERR_PIN_UNREACHABLE);
}

static void test_apply_on_off_on(void)
{
    /* "stream on" / "stream off" / "stream on" (P11.4): the applied route
     * holds its core until routing_clear() - which chain_stream_off() and
     * the post-apply failure path call - releases it; without the clear the
     * second apply is a core conflict and reaches no driver. */
    routing_clear();
    stubs_reset(true);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);      /* on  */
    CHECK_EQ(stub_setup_calls, 1);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_ERR_CORE_IN_USE);  /* on again, no off */
    CHECK_EQ(stub_setup_calls, 1);                        /* refused before any call */
    CHECK_EQ(stub_restore_calls, 0);

    routing_clear();                                      /* off */
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);      /* on  */
    CHECK_EQ(stub_setup_calls, 2);
    CHECK_EQ(stub_restore_calls, 0);

    /* the custom-input shape after the test shape, and back, the same way */
    routing_clear();
    route_t ext = { .src = ROUTE_SRC_EXT, .core = 5u, .pinsel = ROUTE_PINSEL_VREF,
                    .sink = ROUTE_SINK_STREAM, .samc = 3u };
    CHECK_EQ(routing_apply(&ext), ROUTE_OK);
    CHECK(!stub_test_dac);
    routing_clear();
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);
    CHECK(stub_test_dac);
}

/* ---- P11.5: routing_visit(), the "route list" console command's data
 * source - checked here through the same route_visit_t callback the
 * console side turns into "key: value" lines, so the test never needs a
 * console either. ---- */
#define VISIT_MAX 20
static struct {
    const char     *name;
    uint32_t        v;
    char            s[16];
    route_vis_fmt_t fmt;
} visit_log[VISIT_MAX];
static int visit_count;

static void visit_capture(const char *name, uint32_t v, const char *s, route_vis_fmt_t fmt)
{
    CHECK(visit_count < VISIT_MAX);
    if (visit_count >= VISIT_MAX) { return; }
    visit_log[visit_count].name = name;
    visit_log[visit_count].v    = v;
    visit_log[visit_count].fmt  = fmt;
    visit_log[visit_count].s[0] = '\0';
    if (s != NULL) {
        strncpy(visit_log[visit_count].s, s, sizeof visit_log[visit_count].s - 1u);
    }
    visit_count++;
}

static void test_routing_visit_empty(void)
{
    routing_clear();
    visit_count = 0;
    routing_visit(visit_capture);

    /* the "none" line, then the resource table - 0 used throughout */
    CHECK_EQ(visit_count, 11);
    CHECK_EQ(visit_log[0].fmt, ROUTE_VIS_LINE);
    CHECK(strcmp(visit_log[0].name, "route: none - no route active") == 0);
    CHECK(strcmp(visit_log[1].name, "dma_used") == 0);          CHECK_EQ(visit_log[1].v, 0u);
    CHECK(strcmp(visit_log[2].name, "dma_total") == 0);         CHECK_EQ(visit_log[2].v, ROUTE_DMA_CHANNELS);
    CHECK(strcmp(visit_log[3].name, "sccp_used") == 0);         CHECK_EQ(visit_log[3].v, 0u);
    CHECK(strcmp(visit_log[4].name, "sccp_total") == 0);        CHECK_EQ(visit_log[4].v, ROUTE_SCCP_COUNT);
    CHECK(strcmp(visit_log[5].name, "dac_outputs_used") == 0);  CHECK_EQ(visit_log[5].v, 0u);
    CHECK(strcmp(visit_log[6].name, "dac_outputs_total") == 0); CHECK_EQ(visit_log[6].v, ROUTE_DAC_OUTPUTS);
    CHECK(strcmp(visit_log[7].name, "uref_used") == 0);         CHECK_EQ(visit_log[7].v, 0u);
    CHECK(strcmp(visit_log[8].name, "uref_total") == 0);        CHECK_EQ(visit_log[8].v, ROUTE_UREF_COUNT);
    CHECK(strcmp(visit_log[9].name, "ram_used") == 0);          CHECK_EQ(visit_log[9].v, 0u);
    CHECK(strcmp(visit_log[10].name, "ram_budget") == 0);       CHECK_EQ(visit_log[10].v, ROUTE_RAM_BUDGET_BYTES);
}

static void test_routing_visit_active_route(void)
{
    routing_clear();
    stubs_reset(true);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);   /* DAC_PIN/5/3/dac 2/STREAM/samc 0 */

    visit_count = 0;
    routing_visit(visit_capture);
    CHECK_EQ(visit_count, 17);   /* 7 route fields + the 10 resource fields */

    CHECK(strcmp(visit_log[0].name, "route") == 0);
    CHECK_EQ(visit_log[0].fmt, ROUTE_VIS_NUM);
    CHECK_EQ(visit_log[0].v, 0u);

    CHECK(strcmp(visit_log[1].name, "src") == 0);
    CHECK_EQ(visit_log[1].fmt, ROUTE_VIS_STR);
    CHECK(strcmp(visit_log[1].s, "DAC_PIN") == 0);

    CHECK(strcmp(visit_log[2].name, "core") == 0);   CHECK_EQ(visit_log[2].v, 5u);
    CHECK(strcmp(visit_log[3].name, "pinsel") == 0); CHECK_EQ(visit_log[3].v, 3u);
    CHECK(strcmp(visit_log[4].name, "dac") == 0);    CHECK_EQ(visit_log[4].v, 2u);
    CHECK(strcmp(visit_log[5].name, "samc") == 0);   CHECK_EQ(visit_log[5].v, 0u);

    CHECK(strcmp(visit_log[6].name, "sink") == 0);
    CHECK_EQ(visit_log[6].fmt, ROUTE_VIS_STR);
    CHECK(strcmp(visit_log[6].s, "STREAM") == 0);

    /* one ADC-consuming route, no table: 1 DMA channel, 1 shared SCCP, one
     * DAC pin (dac 2), no UREF, one channel's worth of RAM */
    CHECK(strcmp(visit_log[7].name, "dma_used") == 0);           CHECK_EQ(visit_log[7].v, 1u);
    CHECK(strcmp(visit_log[8].name, "dma_total") == 0);          CHECK_EQ(visit_log[8].v, ROUTE_DMA_CHANNELS);
    CHECK(strcmp(visit_log[9].name, "sccp_used") == 0);          CHECK_EQ(visit_log[9].v, 1u);
    CHECK(strcmp(visit_log[10].name, "sccp_total") == 0);        CHECK_EQ(visit_log[10].v, ROUTE_SCCP_COUNT);
    CHECK(strcmp(visit_log[11].name, "dac_outputs_used") == 0);  CHECK_EQ(visit_log[11].v, 1u);
    CHECK(strcmp(visit_log[12].name, "dac_outputs_total") == 0); CHECK_EQ(visit_log[12].v, ROUTE_DAC_OUTPUTS);
    CHECK(strcmp(visit_log[13].name, "uref_used") == 0);         CHECK_EQ(visit_log[13].v, 0u);
    CHECK(strcmp(visit_log[14].name, "uref_total") == 0);        CHECK_EQ(visit_log[14].v, ROUTE_UREF_COUNT);
    CHECK(strcmp(visit_log[15].name, "ram_used") == 0);          CHECK_EQ(visit_log[15].v, ROUTE_CHANNEL_BYTES);
    CHECK(strcmp(visit_log[16].name, "ram_budget") == 0);        CHECK_EQ(visit_log[16].v, ROUTE_RAM_BUDGET_BYTES);

    routing_clear();
}

static void test_apply_setup_failure(void)
{
    /* the clock tree/DAC refused: restore once, SETUP, and the route is not
     * recorded - the same route applies cleanly on the next try */
    routing_clear();
    stubs_reset(false);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_ERR_SETUP);
    CHECK_EQ(stub_setup_calls, 1);
    CHECK_EQ(stub_restore_calls, 1);

    stubs_reset(true);
    CHECK_EQ(routing_apply(&stream_like), ROUTE_OK);
    CHECK_EQ(stub_setup_calls, 1);
    CHECK_EQ(stub_restore_calls, 0);
}

int main(void)
{
    test_route_pin_reachable();
    test_pin_reachability();
    test_core_exclusivity();
    test_dma_limit();
    test_sccp_limit();
    test_dac_outputs();
    test_uref();
    test_ram_budget();
    test_not_yet();

    test_apply_stream();
    test_apply_ext();
    test_apply_precheck_refuses_before_any_call();
    test_apply_not_yet_shapes();
    test_internal_channels_pass_the_rules();
    test_apply_on_off_on();
    test_apply_setup_failure();

    test_routing_visit_empty();
    test_routing_visit_active_route();
    test_generator_claim();

    return check_summary();
}

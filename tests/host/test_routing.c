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

    return check_summary();
}

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

#include "routing.h"
#include "check.h"

static void test_route_pin_reachable(void)
{
    /* core 1 and core 5 reach PINSEL 0..4 only (tools/gen_route_pins.py's
     * route_pin_mask, 0x1F); core 2, 3, 4 reach 0..5 (0x3F). */
    CHECK(route_pin_reachable(1u, 0u));
    CHECK(route_pin_reachable(1u, 4u));
    CHECK(!route_pin_reachable(1u, 5u));
    CHECK(route_pin_reachable(2u, 5u));
    CHECK(!route_pin_reachable(2u, 6u));

    /* PINSEL 7 (UREF) is reachable from every core - DS70005591D Table
     * 16-2, board.h's DAC_UREF_PINSEL comment. */
    for (uint32_t core = 1u; core <= ROUTE_ADC_CORES; core++) {
        CHECK(route_pin_reachable((uint8_t)core, ROUTE_PINSEL_UREF));
    }

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

    return check_summary();
}

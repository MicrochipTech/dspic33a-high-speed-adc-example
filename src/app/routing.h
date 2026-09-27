/*
 * routing.h - the routing core: data model and conflict/resource checks for
 * signal paths (docs/DESIGN-MULTICHANNEL.md section 4.4). P11.1 (27.09.2026):
 * types and routing_add()/routing_clear() only - no routing_apply() yet, so
 * this header pulls in no device header and no driver header, and compiles
 * with a host gcc (tests/host/test_routing.c does exactly that).
 *
 * A route describes one signal path: where its ADC-side signal comes from
 * (route_src_t), which core/pin it uses to get there, and where its data
 * goes (route_sink_t). routing_add() checks a candidate route against every
 * route already added and, if it fits, records it; it never writes a
 * register - that is routing_apply()'s job (P11.3), over the drivers, not
 * here.
 *
 * Resource model (docs/DESIGN-MULTICHANNEL.md section 2, the ATDF of
 * dsPIC33AK-MP_DFP 1.4.260):
 *
 *   - 5 ADC cores (ADC1..ADC5), one route each.
 *   - 8 DMA channels: one per ADC-consuming route (anything but
 *     ROUTE_SRC_RAM_TABLE) plus one per route that ALSO plays a
 *     signal-generator table to a DAC (`table_samples > 0`, independent of
 *     `src`) - "active ADC channels + table DACs <= 8".
 *   - 8 SCCP: one, shared, ADC trigger (every ADC-consuming route rides the
 *     same one - requirement A1, "all ADCs run on the same clock") plus one
 *     playback clock per table-DAC route (no rate-sharing modelled yet -
 *     conservative, see routing.c).
 *   - 2 DAC output pins (DACOUT1/2): one per ROUTE_SRC_DAC_PIN route.
 *   - 1 UREF: one per ROUTE_SRC_DAC_INT route.
 *   - a RAM budget: docs/DESIGN-MULTICHANNEL.md section 2's ~64 KB total,
 *     minus stack/console, in ROUTE_RAM_BUDGET_BYTES - see routing.c.
 *
 * What N+1 restricts beyond the resource model: exactly one sink is wired
 * up, ROUTE_SINK_STREAM - what "stream on"/"stream grab"/"blk" already do.
 * A route that fits every resource limit but asks for ROUTE_SINK_RAM or
 * ROUTE_SINK_CONSOLE is refused with ROUTE_ERR_NOT_YET: those sinks exist in
 * the data model (docs/DESIGN-MULTICHANNEL.md 4.4 lists all three) but
 * nothing reads them yet. Deliberately NOT gated on `src`, `dac` or
 * `table_samples`: those still count fully against the resource table even
 * for a sink that will eventually be ROUTE_SINK_STREAM, so the resource
 * rules above (DMA/SCCP/outputs/UREF/RAM) are exercised by
 * tests/host/test_routing.c exactly as a later, fuller N+2 build would run
 * them - only the "which sink actually does something" boundary is N+1's
 * own.
 *
 * Not included yet, and why: `dsp_chain_t chain` (docs/DESIGN-MULTICHANNEL.md
 * 4.4's route_t sketch has one) is left out because src/dsp_run/ does not
 * exist in N+1 (lib/goertzel, lib/iir1, lib/detect are compiled but called
 * from nowhere - docs/IMPLEMENTATION-PLAN.md P3.7); adding a field with
 * nothing to put in it would only invite an unused warning or a dummy value.
 * A "one DAC in one role at a time" check (docs/DESIGN-MULTICHANNEL.md
 * 4.4's "a DAC as generator and as triangle at the same time?") is not
 * enforced either: ROUTE_UREF_COUNT/ROUTE_DAC_OUTPUTS/the DMA limit already
 * bound how many DACs can be in play at once in N+1, and nothing yet reads
 * the `dac` field to say two routes chose the *same* DAC number for two
 * different roles.
 */
#ifndef ROUTING_H
#define ROUTING_H

#include <stdbool.h>
#include <stdint.h>

/* ---- resource table (docs/DESIGN-MULTICHANNEL.md section 2) ---- */

#define ROUTE_ADC_CORES        5u   /* ADC1..ADC5                          */
#define ROUTE_DMA_CHANNELS     8u   /* DMA0..DMA7                          */
#define ROUTE_SCCP_COUNT       8u   /* SCCP1..SCCP8                        */
#define ROUTE_DAC_COUNT        8u   /* DAC1..DAC8 (CMP_DAC1..8)            */
#define ROUTE_DAC_OUTPUTS      2u   /* DACOUT1, DACOUT2                    */
#define ROUTE_UREF_COUNT       1u   /* one DAC on UREF at a time           */

/* RAM budget: docs/DESIGN-MULTICHANNEL.md section 2's example table sizes a
 * channel's ping-pong pair at 2 x 1024 samples x 2 bytes = 4096 bytes, and
 * the ~64 KB total minus ~8 KB stack/console leaves ~56 KB for buffers and
 * signal-generator tables. Both are runtime checks (decision of 26.09.2026,
 * docs/DESIGN-MULTICHANNEL.md 4.4/section 7), not `_Static_assert` yet: a
 * route's cost is only known once it is added, not at compile time. */
#define ROUTE_HALF_SAMPLES      1024u
#define ROUTE_SAMPLE_BYTES      2u
#define ROUTE_CHANNEL_BYTES     (2u * ROUTE_HALF_SAMPLES * ROUTE_SAMPLE_BYTES)
#define ROUTE_RAM_BUDGET_BYTES  (56u * 1024u)

/* PINSEL 7 is not a package pin - it is UREF, reachable from every core
 * (board.h's DAC_UREF_PINSEL comment, DS70005591D Table 16-2). */
#define ROUTE_PINSEL_UREF       7u

/* RAM_TABLE has no ADC core. */
#define ROUTE_CORE_NONE         0u

/* route_src_t: where a route's ADC-side signal comes from
 * (docs/DESIGN-MULTICHANNEL.md 4.4). */
typedef enum {
    ROUTE_SRC_EXT,       /* external pin on the ADC core: EXT(core, pinsel) */
    ROUTE_SRC_DAC_INT,   /* DAC -> UREF -> ANn7 of a core, on-chip           */
    ROUTE_SRC_DAC_PIN,   /* DAC to its own pin, wired externally to an ADC
                          * pin (like RA8 in the chain test)                */
    ROUTE_SRC_RAM_TABLE  /* the signal generator's table straight into
                          * processing, without ADC or DMA                 */
} route_src_t;

/* route_sink_t: where a route's data goes (docs/DESIGN-MULTICHANNEL.md 4.4:
 * "Sinks: RAM only ..., GUI stream ..., console"). Only ROUTE_SINK_STREAM is
 * wired up in N+1 - see routing.h's top comment and routing_add() in
 * routing.c. */
typedef enum {
    ROUTE_SINK_RAM,     /* counters/results only, no live output           */
    ROUTE_SINK_CONSOLE, /* human-readable text on the console              */
    ROUTE_SINK_STREAM   /* the GUI/console binary stream - stream on/off/
                         * grab, blk: what N+1 actually runs               */
} route_sink_t;

/* route_err_t: one code per rejection reason, so a caller (and a test) can
 * tell which check failed rather than just "no". */
typedef enum {
    ROUTE_OK = 0,
    ROUTE_ERR_PIN_UNREACHABLE, /* the core cannot reach the requested pin   */
    ROUTE_ERR_CORE_IN_USE,     /* the ADC core is already in another route  */
    ROUTE_ERR_SCCP_LIMIT,      /* trigger + playback clocks > SCCP count    */
    ROUTE_ERR_DMA_LIMIT,       /* ADC-consuming routes + routes playing a
                                * table > DMA channel count                 */
    ROUTE_ERR_DAC_OUTPUTS,     /* more than ROUTE_DAC_OUTPUTS DACs external */
    ROUTE_ERR_UREF_BUSY,       /* a second DAC on UREF at the same time     */
    ROUTE_ERR_RAM_BUDGET,      /* halves x channels + tables over budget    */
    ROUTE_ERR_NOT_YET,         /* every check above passed, but this sink is
                                * not wired up yet in N+1                   */
    ROUTE_ERR_TABLE_FULL       /* routing.c's own route_t storage is full   */
} route_err_t;

/* One signal path. `core`/`pinsel` are meaningful (and checked) for every
 * src except ROUTE_SRC_RAM_TABLE, which leaves core at ROUTE_CORE_NONE.
 * `dac` names the DAC 1..8 for ROUTE_SRC_DAC_INT, ROUTE_SRC_DAC_PIN, and for
 * a route that plays a table (below), 0 otherwise.
 *
 * `table_samples`, deliberately independent of `src` and `sink`: > 0 means
 * this route ALSO plays a signal-generator table to `dac` via DMA (its own
 * DMA channel and SCCP playback clock, and the table's RAM) -
 * docs/DESIGN-MULTICHANNEL.md section 6's loop-back self-test ("The signal
 * generator -> DAC -> UREF -> ADC in a loop") is exactly a
 * ROUTE_SRC_DAC_INT route with table_samples > 0: the same DAC is both
 * played and read back in one route. For ROUTE_SRC_RAM_TABLE, table_samples
 * is the size of the table that IS the route's input; there is no ADC/DMA
 * reading it, but if it is also being played out (the ordinary
 * signal-generator case) it still needs the DMA/SCCP/RAM a playback table
 * needs, which is exactly what this field already charges for. 0 when no
 * table is involved. */
typedef struct {
    route_src_t  src;
    uint8_t      core;
    uint8_t      pinsel;
    uint8_t      dac;
    route_sink_t sink;
    uint32_t     table_samples;
} route_t;

/* Every ADC core reaches PINSEL 7 (UREF); for any other PINSEL, whether
 * `core` reaches it as a package pin is a silicon fact, not a policy - see
 * tools/gen_route_pins.py and routing.c's route_pin_mask[]. */
bool route_pin_reachable(uint8_t core, uint8_t pinsel);

/* Removes every route added so far and resets every resource counter. */
void routing_clear(void);

/* Checks `r` against the resource table and every route already added
 * (routing_clear() clears that history). On ROUTE_OK, `r` is recorded and
 * counts against the resource table for the next call; on any other
 * return, nothing changed. */
route_err_t routing_add(const route_t *r);

#endif /* ROUTING_H */

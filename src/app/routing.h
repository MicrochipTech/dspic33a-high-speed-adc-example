/*
 * routing.h - the routing core: data model and conflict/resource checks for
 * signal paths (docs/DESIGN-MULTICHANNEL.md section 4.4). P11.1 (27.09.2026):
 * types and routing_add()/routing_clear(); P11.3 (27.09.2026): routing_apply()
 * for the one route N+1 runs, ROUTE_STREAM. This header still pulls in no
 * device header and no driver header, and compiles with a host gcc
 * (tests/host/test_routing.c does exactly that, with two acquisition.c
 * functions stubbed - see routing.c's top comment).
 *
 * A route describes one signal path: where its ADC-side signal comes from
 * (route_src_t), which core/pin it uses to get there, and where its data
 * goes (route_sink_t). routing_add() checks a candidate route against every
 * route already added and, if it fits, records it; it never writes a
 * register. routing_apply() (P11.3) runs the same checks and then brings
 * the path up through acquisition.c - it touches no register itself either;
 * the drivers do, in the fixed order routing_apply()'s comment in routing.c
 * lays out.
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
 * channel's ping-pong pair at 2 x 1024 samples x 2 bytes = 4096 bytes (2 x 2048
 * = 8192 bytes since 01.10.2026, the buffer capture.c really allocates), and
 * the ~64 KB total minus ~8 KB stack/console leaves ~56 KB for buffers and
 * signal-generator tables. Both are runtime checks (decision of 26.09.2026,
 * docs/DESIGN-MULTICHANNEL.md 4.4/section 7), not `_Static_assert` yet: a
 * route's cost is only known once it is added, not at compile time. */
#define ROUTE_HALF_SAMPLES      2048u   /* capture.h's SAMPLES_PER_HALF_MAX since 01.10.2026 */
#define ROUTE_SAMPLE_BYTES      2u
#define ROUTE_CHANNEL_BYTES     (2u * ROUTE_HALF_SAMPLES * ROUTE_SAMPLE_BYTES)
#define ROUTE_RAM_BUDGET_BYTES  (56u * 1024u)

/* Two PINSEL values are not package pins but internal channels every core
 * has (DS70005591D Table 16-2; the pack's ATDF, dsPIC33AK512MPS512.atdf
 * and ...MPS506.atdf, ADC instance params: "ADnAN6 = 15/16*VDD Reference
 * Input", "ADnAN7 = Uref Input" for n = 1..5): PINSEL 6 is the reference
 * the self-test samples (capture.c), PINSEL 7 is UREF (board.h's
 * DAC_UREF_PINSEL). Core 5 has two more, in routing.c's table. */
#define ROUTE_PINSEL_VREF       6u
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
    ROUTE_ERR_NOT_YET,         /* every check above passed, but this sink -
                                * or, in routing_apply(), this route shape -
                                * is not wired up yet in N+1                */
    ROUTE_ERR_TABLE_FULL,      /* routing.c's own route_t storage is full   */
    ROUTE_ERR_SETUP,           /* routing_apply() only: the checks passed but
                                * the clock tree, trigger clock or DAC
                                * refused (acq_chain_setup_input() returned
                                * false); the boot configuration is back    */
    ROUTE_ERR_DAC_BUSY         /* SG.5: the DAC this route (or the signal
                                * generator) wants is already the other's   */
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
 * table is involved.
 *
 * `samc` (P11.3): the ADC sample time of the core's channel, the value
 * adc_init() writes to ADnCH0CON1.SAMC (0 = 0.5 TAD, the shortest; the
 * citation is at that write in adc.c) - how the core samples this pin, so
 * part of the source
 * description, not a rate: the sample RATE is not in a route at all.
 * Requirement A1 (docs/DESIGN-MULTICHANNEL.md: "all ADCs run on the same
 * clock") makes it one shared trigger period for every ADC-consuming
 * route, set by the caller after routing_apply() - see routing_apply(). */
typedef struct {
    route_src_t  src;
    uint8_t      core;
    uint8_t      pinsel;
    uint8_t      dac;
    uint8_t      samc;          /* beside the other bytes: 16 bytes a route,
                                 * not 20 - routing.c keeps 24 of them      */
    route_sink_t sink;
    uint32_t     table_samples;
} route_t;

/* The two paths this firmware runs today, as data (P11.3, 27.09.2026).
 * Defined in acquisition.c, next to the code that runs them, because their
 * core/pin come from board.h's macros (DAC_ADC_CORE/DAC_ADC_PINSEL,
 * ADC_INSTANCE/ADC_PINSEL/ADC_SAMC) - the same macros chain_stream_on() and
 * acq_chain_restore() already use, so the data cannot drift from the code,
 * and both boards get their own values without this header (which the host
 * test compiles) including board.h.
 *
 *   ROUTE_STREAM  SCCP1 -> ADC core 5 (Single mode) -> DMA0 -> ping-pong ->
 *                 CPU, DAC2 on its pin (RA8 = AD5AN3) as the signal: the
 *                 chain "stream on <ksps>" runs, docs/ANALYSIS.md C.8.
 *   ROUTE_B2B     the back-to-back capture on the board's default core and
 *                 input, one channel: what "snap"/"blk"/"test ..." run.
 *                 Data only in N+1 - the `test` suite keeps its own path;
 *                 switching it to routing_apply(&ROUTE_B2B) is N+2's. */
extern const route_t ROUTE_STREAM;
extern const route_t ROUTE_B2B;

/* Every ADC core reaches PINSEL 6 and 7 (the internal reference and UREF,
 * above); core 5 also reaches its two further internal channels (PINSEL 5,
 * "Touch ADC Input", and 8, VDDCORE - the ATDF's own names, see routing.c);
 * for any other PINSEL, whether `core` reaches it as a package pin is a
 * silicon fact, not a policy - see tools/gen_route_pins.py and routing.c's
 * route_pin_mask[]. PINSEL 9..15 select nothing the ATDF names on either
 * device and are refused. */
bool route_pin_reachable(uint8_t core, uint8_t pinsel);

/* Removes every route added so far and resets every resource counter. */
void routing_clear(void);

/* Checks `r` against the resource table and every route already added
 * (routing_clear() clears that history). On ROUTE_OK, `r` is recorded and
 * counts against the resource table for the next call; on any other
 * return, nothing changed. */
route_err_t routing_add(const route_t *r);

/* Checks `r` exactly as routing_add() would (every rule, the same order,
 * against the routes already added), then brings the path up through
 * acquisition.c in the fixed order docs/DESIGN-MULTICHANNEL.md 4.4 gives -
 * DMA off, cores off, clock and trigger, cores on, DMA from scratch (see
 * routing.c for which call is which step) - and records the route. Nothing
 * converts yet when it returns: the trigger (SCCP1) is stopped and the DMA
 * channel is down, exactly where acq_chain_setup() leaves them, and the
 * caller starts the shared trigger at its rate (capture_chain_start(), as
 * chain_stream_on_input() does). Only routes whose shape acquisition.c can
 * run today are applied: ROUTE_SINK_STREAM, no table, src ROUTE_SRC_EXT
 * (any core/pin, the DAC left alone - "stream on <ksps> <core> <pinsel>")
 * or ROUTE_SRC_DAC_PIN with dac 2 (DAC2 started at mid-scale first -
 * ROUTE_STREAM); every other shape returns ROUTE_ERR_NOT_YET before any
 * driver is called, as does any conflict with a route already added. On
 * ROUTE_ERR_SETUP the boot configuration has been restored
 * (acq_chain_restore()) and the route is not recorded. */
route_err_t routing_apply(const route_t *r);

/* route_vis_fmt_t/route_visit_t (P11.5, 27.09.2026): routing_visit() hands
 * the active route(s) and the resource table to this callback, one field
 * per call, exactly the same print-free pattern port/regs.h's reg_visit_t
 * uses for a driver's register dump - routing.c stays free of any print
 * call and tests/host/test_routing.c can check the calls a route makes
 * without a console.
 *
 *   ROUTE_VIS_NUM   name is the key, v the value (decimal), s ignored.
 *   ROUTE_VIS_STR   name is the key, s the value (a short, static string
 *                   naming an enum - route_src_t/route_sink_t), v ignored.
 *   ROUTE_VIS_LINE  name is the whole line, v and s ignored - only used
 *                   for "no route active" when nothing has been recorded.
 *
 * The console command this drives, "route list" (cli.c), turns every call
 * into one "key: value" line - the same shape every other command's reply
 * already uses (put_kv()/put_line()) - so its console.h/git-committed
 * examples, board_run.py's `route list` for the first board run
 * (docs/IMPLEMENTATION-PLAN.md phase BR, block R6) and tests/smoke/
 * expected.log all read the same lines this function defines the order
 * of. */
typedef enum {
    ROUTE_VIS_NUM,
    ROUTE_VIS_STR,
    ROUTE_VIS_LINE
} route_vis_fmt_t;

typedef void (*route_visit_t)(const char *name, uint32_t v, const char *s, route_vis_fmt_t fmt);

/* Hands out, in this fixed order: with no route recorded, one ROUTE_VIS_LINE
 * ("route: none - ..."); otherwise, for every recorded route in the order
 * routing_add()/routing_apply() added them, "route" (its index, NUM), then
 * "src"/"core"/"pinsel"/"dac"/"samc"/"sink" (STR for src and sink, NUM for
 * the rest) - route_t's own fields, routing.h's comment on route_t names
 * what each means. After every route (including when there is none), the
 * resource table: "dma_used"/"dma_total", "sccp_used"/"sccp_total",
 * "dac_outputs_used"/"dac_outputs_total", "uref_used"/"uref_total",
 * "ram_used"/"ram_budget" (NUM throughout) - the same counters route_check()
 * charges a candidate route against, read back rather than recomputed
 * twice. Never writes a register, never prints anything itself. */
void routing_visit(route_visit_t visit);

/* The signal generator's claim (SG.5, 29.09.2026): one DMA channel, one
 * SCCP, the DAC's output pin and n x 2 bytes of table, held apart from
 * the route table so that routing_clear() ("stream off") leaves it alone.
 * routing_gen_check() says whether it fits - ROUTE_ERR_DAC_BUSY when a
 * recorded route already uses that DAC (ROUTE_STREAM's DAC2 triangle),
 * the resource errors otherwise - without claiming; routing_gen_claim()
 * checks and claims; routing_gen_release() gives it back. While claimed,
 * routing_add()/routing_apply() refuse a route on the same DAC with
 * ROUTE_ERR_DAC_BUSY and count the claim against every other limit, and
 * routing_visit() reports it ("generator_dac"/"generator_n" before the
 * resource table; nothing extra when no generator runs). */
route_err_t routing_gen_check(uint8_t dac, uint32_t n);
route_err_t routing_gen_claim(uint8_t dac, uint32_t n);
void        routing_gen_release(void);

#endif /* ROUTING_H */

/*
 * regs.c (scenario, P0.5) - regs_dump(), diag.c compiled unchanged.
 *
 * The plan's only entry point is regs_dump() itself, called straight from
 * the reset state (every SFR at its ATDF reset value, sfr_reset[], as
 * trace_begin() sets up since P0.9 - all 0 before that) - this scenario
 * is about the DUMP'S OWN OUTPUT FORMAT (the order it visits each module,
 * the "name: value" / "name: 0x........" lines, all routed through the
 * stubbed console as `C` lines per decision 1), not about reproducing a
 * booted system's register values - `boot`, `b2b`, `variants`, `clk` and
 * `stream_on(_input)` already exercise those. Because nothing before it
 * has run, every register regs_dump() reads is still at its reset value
 * (which makes this golden, as a side effect, a printed list of the ATDF
 * reset values of every register the dump covers) and none of its
 * callees waits on anything, so no hardware-model rules are needed here.
 *
 * regs_dump() (diag.c) calls, in order: clock_regs_dump(), adc_regs_dump(),
 * dma0_regs_dump(), capture_regs_dump(), dac_regs_dump(), then
 * console_regs_dump() and three direct reads of its own (INTCON1, PCTRAP,
 * fail_code) - hence diag.c, clock.c, adc.c, dma.c, capture.c, dac.c,
 * sccp.c (capture.c's capture_variant_regs() calls sccp1_regs_dump(),
 * so sccp.c must link even though regs_dump() itself never reaches that
 * function - the linker resolves every symbol a linked .o references),
 * timebase.c and led.c (capture.c calls into both) round out the sources.
 *
 * diag.c is linked here (unlike every other P0.5 scenario, via
 * -DHAVE_DIAG in regs.cflags): its real fail_code/boot_stage/chain_mark
 * and its real fail() take over from stubs.c's own copies, which compile
 * out under HAVE_DIAG (see stubs.c's file header). Nothing in this
 * scenario calls fail(), so diag.c's real one - which never returns - is
 * never exercised; it only needs to LINK, which is why stubs.c also
 * supplies console_sync_baud() and console_regs_dump() (cli.c-only
 * functions diag.c's fail() and regs_dump() reference).
 */
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "diag.h"
#include "capture.h"

int main(void)
{
    trace_begin("regs");
    /* capture_regs_dump() (part of regs_dump()) prints "buf" as a raw
     * address; registering it turns that into the same "&dma_buffer+0x0"
     * form every other scenario uses (tests/trace/README.md's open point
     * on capture.c's static dma_buffer, closed the same way as `boot`'s). */
    trace_region(capture_buffer(), SAMPLES_PER_BUF_MAX * sizeof(uint16_t), "dma_buffer");

    trace_point("before regs_dump()");
    regs_dump();
    trace_point("after regs_dump()");

    trace_end();
    return 0;
}

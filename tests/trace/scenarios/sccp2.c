/*
 * sccp2.c (scenario, SG.2, 29.09.2026) - the signal generator's playback
 * clock: sccp2_start() in both pace modes, sccp.c compiled unchanged.
 *
 *   sccp2_start(1000, TMR16)     100 kHz from the 100 MHz peripheral clock:
 *                                dual 16-bit timer, the period in PRH, no
 *                                prescaler - the mode the board confirmed
 *                                on 29.09.2026 (100 000 DMA transfers/s)
 *   sccp2_start(100000, TMR16)   1 kHz: 100 000 clocks do not fit 16 bits,
 *                                so TMRPS = 1:4 and PRH = 24999
 *   sccp2_start(1000, OC32)      the 32-bit output-compare candidate, kept
 *                                for the record (0 transfers on the board)
 *   sccp2_stop()
 *   sccp2_start(1, TMR16)        refused before any write
 *
 * DMA channel 1 and the shared window are not in this scenario:
 * dma_tx_start() checks its table against the device's RAM range, which
 * a host array does not lie in. The `stream_on` goldens (unchanged by SG.1)
 * cover dma0_init()'s side with channel 1 idle, the board run the rest.
 */
#include <stdbool.h>
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "hwmodel.h"
#include "sccp.h"

int main(void)
{
    trace_begin("sccp2");
    hwmodel_start(NULL, 0u, 0u);

    bool ok;
    ok = sccp2_start(1000u, SCCP2_PACE_TMR16);
    trace_note("# sccp2_start(1000, TMR16) -> %d, period %u, prescaled\n",
               (int)ok, (unsigned)sccp2_period());
    trace_point("TMR16 100 kHz: dual 16-bit timer, PRH = 999, 1:1");

    ok = sccp2_start(100000u, SCCP2_PACE_TMR16);
    trace_note("# sccp2_start(100000, TMR16) -> %d, period %u\n",
               (int)ok, (unsigned)sccp2_period());
    trace_point("TMR16 1 kHz: TMRPS 1:4, PRH = 24999");

    ok = sccp2_start(1000u, SCCP2_PACE_OC32);
    trace_note("# sccp2_start(1000, OC32) -> %d\n", (int)ok);
    trace_point("OC32 100 kHz: 32-bit output compare, CCP2RB = 500");

    sccp2_stop();
    trace_point("sccp2_stop()");

    ok = sccp2_start(1u, SCCP2_PACE_TMR16);
    trace_note("# sccp2_start(1, TMR16) -> %d (refused, nothing written)\n", (int)ok);
    trace_point("refused");

    hwmodel_stop();
    trace_end();
    return 0;
}

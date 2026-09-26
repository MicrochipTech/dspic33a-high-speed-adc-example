/*
 * timebase.c (scenario) - P0.4 working example: timebase_init() and its
 * companions, timebase.c compiled unchanged. Not a golden trace yet
 * (P0.5 records it); this file is what P0.4's acceptance trace is taken
 * from - the net writes must be T1CON 0x0 -> 0x8010, PR1 0x0 ->
 * 0xFFFFFFFF, and no TMR1 line (0 -> 0 is invisible to a snapshot diff,
 * tests/trace/README.md).
 */
#include <stdint.h>
#include <xc.h>
#include "sfr_table.h"
#include "recorder.h"
#include "timebase.h"

int main(void)
{
    trace_begin("timebase");

    trace_point("timebase_init()");
    timebase_init();

    trace_point("timebase_init() again - already running, no writes expected");
    timebase_init();

    trace_point("timebase_check()");
    uint32_t t = timebase_check();
    trace_note("# -> %lu ticks\n", (unsigned long)t);

    trace_point("timebase_ticks() twice");
    uint32_t a = timebase_ticks();
    uint32_t b = timebase_ticks();
    trace_note("# -> difference %lu\n", (unsigned long)(b - a));

    trace_end();
    return 0;
}

/*
 * b2b_link.c - "snap", "rate" and "blk": the binary block transfer of the
 * back-to-back burst mode (docs/PLAN-BINARY-TRANSFER.md). Lab, not core:
 * moved out of gui_link.c on CORE.2 (02.10.2026) unchanged, because it
 * goes through meter.c's capture_oneshot() - the burst mode the GUI no
 * longer uses. gui_link.c keeps "stream grab", the GUI's data path.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "adc.h"
#include "capture.h"
#include "meter.h"     /* the back-to-back instruments (CORE.5) */
#include "clock.h"
#include "timebase.h"
#include "console.h"
#include "cmd_parser.h"
#include "fmt.h"
#include "frame.h"
#include "b2b_link.h"

/* The reply helpers borrowed from cli.c (P6.2's arrangement). */
void put_kv(const char *key, uint32_t v);
void put_line(const char *s);
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out);
void usage(const char *text);

/* ------------------------------------------------------------------ *
 * "snap" - fill the buffer once and stop, for a coherent window at a rate
 * the main loop cannot keep up with (cli.c's original comment, moved
 * verbatim - unchanged, no framing involved). See "dump" (cli.c) for how
 * the filled buffer is then read.
 * ------------------------------------------------------------------ */
static void cmd_snap_body(int argc, char **argv)
{
    (void)argc; (void)argv;
    const uint32_t n     = 2u * capture_half_len();
    const uint32_t t0    = timebase_ticks();
    const uint32_t rc    = capture_oneshot();
    const uint32_t ticks = timebase_ticks() - t0;
    (void)capture_settle();

    if (rc != 0u) {
        put_line(capture_overrun_aborted()
                 ? "snap: stopped by the overrun brake - this rate floods the CPU"
                 : "snap: no data");
        cmd_parser_fail();
        return;
    }
    const uint32_t window_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / TIMEBASE_HZ);
    put_kv("samples", n);
    put_kv("window ns", window_ns);
    put_kv("ksps measured", timebase_ksps(n, ticks));
    put_kv("ksps nominal", capture_nominal_ksps(0u));
    put_kv("overrun during the burst", dma_overrun);
    put_kv("input", capture_pinsel());
    put_kv("adc core", adc_core());
}
/* Polled transmit while it measures (console_quiet_begin(), cli.c). */
static void cmd_snap_fn(int argc, char **argv)
{
    const bool quiet = console_quiet_begin();
    cmd_snap_body(argc, argv);
    console_quiet_end(quiet);
}
CMD_DEFINE(snap, "snap", cmd_snap_fn, "snap - fill the buffer once and stop; then dump it");

/* ------------------------------------------------------------------ *
 * "rate" - the sample rate directly (unchanged, no framing involved).
 * ------------------------------------------------------------------ */
static void cmd_rate_fn(int argc, char **argv)
{
    uint32_t want = 0u, got = 0u;
    if ((argc != 2) || !arg_u32(argv[1], 4000u, 40000u, &want)) {
        usage("rate <4000..40000 ksps>  (the closest the PLL can make; 'rate 8000' gives exactly 8000)");
        return;
    }
    const uint32_t rc = capture_set_rate(want, &got);
    put_kv("ksps asked for", want);
    put_kv("ksps set", got);
    put_kv("pll1 fbdiv", clock_pll1_fbdiv());
    put_kv("pll1 postdiv1", clock_adc_pll_postdiv1());
    put_kv("pll1 postdiv2", clock_adc_pll_postdiv2());
    put_kv("adc clock Hz", clock_adc_hz());
    put_line(clock_adc_div_error(rc));
    if (rc != CLKDIV_OK) { cmd_parser_fail(); }
}
CMD_DEFINE(rate, "rate", cmd_rate_fn, "rate <ksps> - the sample rate, 4000..40000");

/* ------------------------------------------------------------------ *
 * "blk <n>" - the sample block as binary, framed by frame_send()
 * (docs/PLAN-BINARY-TRANSFER.md, frame.h):
 *
 *   BIN n=<count> p1=<postdiv1> p2=<postdiv2> samc=<samc> in=<pinsel>CRLF
 *   <2*count bytes, uint16 little endian, 12-bit value in bits 11:0>
 *   CRLF CRC <hex4> CRLF
 *
 * On failure the header says n=0, frame_send() sends the bare "CRC 0000"
 * tail and no payload, and the command fails - the client's
 * synchronisation on the prompt and ACK/NAK does not change.
 * ------------------------------------------------------------------ */
static void cmd_blk_fn(int argc, char **argv)
{
    const uint32_t total = 2u * capture_half_len();
    uint32_t n = total;
    if ((argc > 2) || ((argc == 2) && !arg_u32(argv[1], 1u, total, &n))) {
        usage("blk [n 1..4096] - the sample block as binary, with a CRC");
        return;
    }

    /* One fresh burst, stopped by the DMA interrupt, so the block is a
     * contiguous window and not a half the DMA is still writing. */
    const uint32_t rc = capture_oneshot();
    (void)capture_settle();

    char head[96];
    char *p = copy_str(head, "BIN n=");
    p = u32_to_str(p, (rc == 0u) ? n : 0u);
    p = copy_str(p, " p1=");   p = u32_to_str(p, clock_adc_pll_postdiv1());
    p = copy_str(p, " p2=");   p = u32_to_str(p, clock_adc_pll_postdiv2());
    p = copy_str(p, " samc="); p = u32_to_str(p, capture_samc());
    p = copy_str(p, " in=");   p = u32_to_str(p, capture_pinsel());
    p = copy_str(p, "\r\n");
    const size_t head_len = (size_t)(p - head);

    if (rc != 0u) {
        (void)frame_send(console_write_raw, cmd_parser_aborted, head, head_len, NULL, 0u);
        cmd_parser_fail();
        return;
    }

    const uint32_t sent = frame_send(console_write_raw, cmd_parser_aborted,
                                      head, head_len, capture_buffer(), n);

    /* A short block is not a protocol error - the client compares the
     * byte count against the header and says so - but it is not a
     * success either. */
    if (sent != (2u * n)) { cmd_parser_fail(); }
}
CMD_DEFINE(blk, "blk", cmd_blk_fn, "blk [n] - the sample block as binary, with a CRC");

/* The binary block transfer: snap/rate/blk. */
void link_register(void)
{
    (void)cmd_register(&cmd_snap);
    (void)cmd_register(&cmd_rate);
    (void)cmd_register(&cmd_blk);
}

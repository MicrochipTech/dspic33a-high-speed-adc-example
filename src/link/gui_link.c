/*
 * gui_link.c - see gui_link.h.
 *
 * "snap", "rate" and "blk" (docs/PLAN-BINARY-TRANSFER.md) moved out of
 * cli.c verbatim; "blk" and "stream grab" (still dispatched from cli.c's
 * cmd_stream_fn(), see gui_link_stream_grab()) rewritten to build their
 * frame through src/lib/frame.c's frame_send() instead of a private copy
 * of the same chunked-payload-plus-CRC loop repeated twice in cli.c. The
 * bytes on the wire are unchanged: frame_send() is exactly cmd_blk_fn()'s
 * and cmd_stream_grab()'s old payload/CRC loop, and the header text built
 * here is exactly the same copy_str()/u32_to_str() sequence cli.c used,
 * only handed to frame_send() instead of cmd_parser_write() directly. The
 * restart-always rule of "stream grab" - chain_stream_grab_end() runs
 * unconditionally after the transfer, whatever the transfer's outcome -
 * is untouched.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "adc.h"
#include "capture.h"
#include "clock.h"
#include "timebase.h"
#include "chaintest.h"
#include "console.h"
#include "cmd_parser.h"
#include "fmt.h"
#include "frame.h"
#include "gui_link.h"

/* Not static: the four small reply helpers that every command in this
 * file was already borrowing from cli.c before the move (P6.2 made the
 * same borrowing arrangement for src/tests/bench.c) - declared extern
 * here instead of duplicated. */
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
static void cmd_snap_fn(int argc, char **argv)
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

/* ------------------------------------------------------------------ *
 * "stream grab" - one halt/transfer/restart cycle for the GUI, framed by
 * frame_send() exactly like "blk" above, with a wider header (see
 * gui_link.h and cli.c's cmd_stream_fn(), which still owns "stream
 * on|off|grab" dispatch and the plain "stream" status report):
 *
 *   GRAB n=<count> from=<from> ksps=<ksps> ov=<overrun> late=<late>
 *        missed=<missed> halves=<halves> xfer=<transfers> slp=<slpdat>
 *        dachz=<dac_hz> proc=<0|1>CRLF
 *   (proc= since 01.10.2026: 1 = the payload is sigproc_block()'s result,
 *   the signal processing was on - sigproc.h)
 *   <2*count bytes, uint16 little endian, 12-bit value in bits 11:0>
 *   CRLF CRC <hex4> CRLF
 *
 * On failure (no stream on, or the halt/restart itself failed) the header
 * says n=0 and every other field 0, frame_send() sends the bare "CRC
 * 0000" tail and no payload, and the command fails - the client's
 * synchronisation on the prompt and ACK/NAK does not change, exactly as
 * for "blk" with a refused block. The restart (chain_stream_grab_end())
 * always runs, whether the transfer went out whole or was cut short by
 * Ctrl+C or a disconnect: a short block is for the client to report, not
 * a reason to leave the chain half-configured (docs/PLAN-BINARY-TRANSFER.md's
 * Ctrl+C risk, the same one "blk" already has to live with). */
void gui_link_stream_grab(void)
{
    chain_grab_t g;
    const bool got = chain_stream_grab_begin(&g);

    /* Longest possible header: the eleven field names with their spaces
     * and "=" (69 characters), ten 32-bit values of up to 10 digits, proc
     * 1 digit, CRLF and the terminator - 69 + 100 + 1 + 2 + 1 = 173. Was
     * char[128] until 01.10.2026, too short for ten fields at their
     * largest even before proc= (165 + 1). */
    char head[176];
    char *p = copy_str(head, "GRAB n=");
    p = u32_to_str(p, got ? g.win_len : 0u);
    p = copy_str(p, " from=");   p = u32_to_str(p, got ? g.from : 0u);
    p = copy_str(p, " ksps=");   p = u32_to_str(p, got ? g.ksps : 0u);
    p = copy_str(p, " ov=");     p = u32_to_str(p, got ? g.overrun : 0u);
    p = copy_str(p, " late=");   p = u32_to_str(p, got ? g.late : 0u);
    p = copy_str(p, " missed="); p = u32_to_str(p, got ? g.missed : 0u);
    p = copy_str(p, " halves="); p = u32_to_str(p, got ? g.halves : 0u);
    p = copy_str(p, " xfer=");   p = u32_to_str(p, got ? g.transfers : 0u);
    p = copy_str(p, " slp=");    p = u32_to_str(p, got ? g.slpdat : 0u);
    p = copy_str(p, " dachz=");  p = u32_to_str(p, got ? g.dac_hz : 0u);
    p = copy_str(p, " proc=");   p = u32_to_str(p, got ? g.proc : 0u);
    p = copy_str(p, "\r\n");
    const size_t head_len = (size_t)(p - head);

    if (!got) {
        (void)frame_send(console_write_raw, cmd_parser_aborted, head, head_len, NULL, 0u);
        cmd_parser_fail();
        return;
    }

    const uint32_t n = g.win_len;
    const uint32_t sent = frame_send(console_write_raw, cmd_parser_aborted,
                                      head, head_len, g.win, n);

    /* Restart unconditionally - see the comment above. */
    const bool resumed = chain_stream_grab_end();
    if ((sent != (2u * n)) || !resumed) { cmd_parser_fail(); }
}

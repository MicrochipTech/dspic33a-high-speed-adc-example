/*
 * gui_link.c - see gui_link.h. Since CORE.2 (02.10.2026) "stream grab"
 * only - the GUI's data path, part of the core; "snap"/"rate"/"blk" moved
 * to the lab's b2b_link.c. What follows is the history of both:
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
#include "sigproc.h"     /* filter and Goertzel selection (02.10.2026) */
#include "clock.h"
#include "timebase.h"
#include "acquisition.h"
#include "console.h"
#include "cmd_parser.h"
#include "fmt.h"
#include "frame.h"
#include "gui_link.h"

/* Not static: the four small reply helpers that every command in this
 * file was already borrowing from cli.c before the move (P6.2 made the
 * same borrowing arrangement for src/lab/bench.c) - declared extern
 * here instead of duplicated. */
void put_kv(const char *key, uint32_t v);
void put_line(const char *s);
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out);
void usage(const char *text);


/* ------------------------------------------------------------------ *
 * "stream grab" - one halt/transfer/restart cycle for the GUI, framed by
 * frame_send() exactly like "blk" above, with a wider header (see
 * gui_link.h and cli.c's cmd_stream_fn(), which still owns "stream
 * on|off|grab" dispatch and the plain "stream" status report):
 *
 *   GRAB n=<count> from=<from> ksps=<ksps> ov=<overrun> late=<late>
 *        missed=<missed> halves=<halves> xfer=<transfers> slp=<slpdat>
 *        dachz=<dac_hz> proc=<0|1> load=<per mille>CRLF
 *   (proc= since 01.10.2026: 1 = the payload is sigproc_block()'s result,
 *   the signal processing was on - sigproc.h; load= since 02.10.2026: the
 *   processing's mean share of a half period since the previous grab, per
 *   mille - 1000 = it just keeps up)
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
/* Polled by frame_send() between two payload chunks of a "stream grab"
 * (64 bytes each): the main loop's work goes on while the frame is
 * built - the stream does not stop for a grab any more (01.10.2026), and
 * the CRC and the copy of a 4 KB pair into the transmit ring take 2-3 ms
 * of this command, 25 halves at 8 MSPS (board, 01.10.2026). Then the
 * Ctrl+C check that frame_send()'s callback always was. */
static bool grab_poll(void)
{
    (void)capture_service();
    return cmd_parser_aborted();
}

void gui_link_stream_grab(void)
{
    chain_grab_t g;
    const bool got = chain_stream_grab_begin(&g);

    /* Longest possible header: the twelve field names with their spaces
     * and "=" (75 characters), ten 32-bit values of up to 10 digits, proc
     * 1 digit, load up to 5 (capped at 99999, acquisition.c), CRLF and the
     * terminator - 75 + 100 + 1 + 5 + 2 + 1 = 184. Was char[128] until
     * 01.10.2026, too short for ten fields at their largest even before
     * proc= (165 + 1); char[176] until load= came (02.10.2026), char[192]
     * until the Goertzel's three fields came the same day: " gz=" and
     * " gzs=" with up to 10 digits each, " gzd=" with 1 - 35 more, 219. */
    char head[224];
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
    p = copy_str(p, " load=");   p = u32_to_str(p, got ? g.load_pm : 0u);
    /* The Goertzel at fs/16 (sigproc.c), only while it runs and once it has
     * seen a block: amplitude (LSB), share of the power (per mille),
     * detected (0/1) - absent otherwise, so the GUI shows it as off. */
    if (got && capture_sigproc_enabled() && sigproc_goertzel_on()) {
        sigproc_gz_t gz;
        sigproc_goertzel_get(&gz);
        if (gz.valid) {
            p = copy_str(p, " gz=");  p = u32_to_str(p, gz.amp);
            p = copy_str(p, " gzs="); p = u32_to_str(p, gz.share_pm);
            p = copy_str(p, " gzd="); p = u32_to_str(p, gz.detected);
        }
    }
    p = copy_str(p, "\r\n");
    const size_t head_len = (size_t)(p - head);

    if (!got) {
        (void)frame_send(console_write_raw, cmd_parser_aborted, head, head_len, NULL, 0u);
        cmd_parser_fail();
        return;
    }

    const uint32_t n = g.win_len;
    const uint32_t sent = frame_send(console_write_raw, grab_poll,
                                      head, head_len, g.win, n);

    /* Restart unconditionally - see the comment above. */
    const bool resumed = chain_stream_grab_end();
    if ((sent != (2u * n)) || !resumed) { cmd_parser_fail(); }
}

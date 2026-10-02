/*
 * gui_link.h - the binary block transfer and the GUI's chain-stream grab
 * cycle (gui_link.c), moved out of cli.c on 27.09.2026 (P6.4)
 *
 * docs/PLAN-BINARY-TRANSFER.md: "snap"/"rate"/"blk" - P6.1's link_register()
 * already grouped them as the binary-transfer commands - and the "stream
 * grab" sub-command cli.c's "stream" still dispatches to (the "on"/"off"/
 * status parts of "stream" stay in cli.c; only the grab cycle's body moved,
 * chaintest.c/.h still owns the halt/resume mechanism itself). Both binary
 * frames go out through src/lib/frame.c now instead of a private copy of
 * the same header/payload/CRC loop cli.c used to carry twice.
 */
#ifndef GUI_LINK_H
#define GUI_LINK_H

/* "snap"/"rate"/"blk" and their link_register() moved to the lab's
 * b2b_link.c/.h on CORE.2 (02.10.2026); this header is the grab only. */

/* "stream grab": one ping-pong pair to the GUI while the stream carries on
 * in the other (since 01.10.2026; a halt/transfer/restart cycle before).
 * Called from cli.c's cmd_stream_fn() for the "grab" sub-command. */
void gui_link_stream_grab(void);

/* The application's fields in the GRAB header (02.10.2026, sigproc.h's
 * application hooks): called while the signal processing is on, appends
 * " key=value" fields at p and returns the new end; `end` is the last
 * position it may write to, terminator included. Weak in gui_link.c,
 * appends nothing. */
char *gui_link_app_fields(char *p, const char *end);

#endif /* GUI_LINK_H */

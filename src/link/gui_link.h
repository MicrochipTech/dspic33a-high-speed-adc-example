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

/* Registers "snap", "rate", "blk" - called from cli_init() at exactly the
 * position link_register() occupied when it was still defined in cli.c. */
void link_register(void);

/* "stream grab": one halt/transfer/restart cycle of the standing chain,
 * for the GUI. Called from cli.c's cmd_stream_fn() for the "grab"
 * sub-command. */
void gui_link_stream_grab(void);

#endif /* GUI_LINK_H */

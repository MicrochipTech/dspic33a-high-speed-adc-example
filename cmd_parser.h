/*
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*******************************************************************************
  Generic Console Command Parser

  File Name:
    cmd_parser.h

  Summary:
    Self-registering, argc/argv-style command dispatcher for a text console
    over any byte stream (UART, USB-CDC, ...).

  Description:
    Ported from the console command framework in
    https://github.com/zabooh/pic32cm-pl10-cnano-zephyr (app/src/cmd_parser.c,
    app/src/cmd.h). That version targets Zephyr and relies on mechanisms not
    available here: console_getchar()/console_init() (Zephyr console
    subsystem), K_THREAD_DEFINE, and a linker-section-based
    STRUCT_SECTION_ITERABLE registry built via a custom .ld fragment. This
    port keeps the same command model - argc/argv handlers, a "help"
    registry, a hand-rolled line editor with bash-style Up/Down history -
    but adapts it to this project's build:

    Line editing (all of it driven by cmd_parser_feed_char(), see the key
    table below). Cursor movement and redraw use plain backspaces only, no
    ANSI cursor positioning, so it also works on a dumb terminal:

      Left / Right      ESC [ D / ESC [ C      move within the line
      Up / Down         ESC [ A / ESC [ B      history back / forward
      Backspace / DEL   0x08 / 0x7F            delete left of the cursor
      Delete            ESC [ 3 ~              delete under the cursor
      Home              ESC [ H, ESC [ 1 ~, Ctrl+A (0x01)
      End               ESC [ F, ESC [ 4 ~, Ctrl+E (0x05)
      Ctrl+U (0x15)     clear the whole line
      Ctrl+K (0x0B)     clear from the cursor to the end of the line
      Ctrl+W (0x17)     delete the word left of the cursor

    Typing a printable character inserts it at the cursor. There is no yank
    buffer (no Ctrl+Y) - it would cost another CMD_PARSER_LINE_MAX_LEN bytes
    of RAM, which this project cannot spare.

    Prompt as a protocol element
    ----------------------------
    The prompt (CMD_PARSER_PROMPT, "> ") is not decoration. It is the statement
    "the previous line has been accepted and fully processed, and I am ready for
    the next one". The module emits it itself, as the last thing it does for a
    submitted line - which, because cmd_parser_write() only returns once the sink
    has taken the bytes, means everything belonging to that command is already on
    the wire when the prompt appears.

    That makes the console usable by a script or an agent, not just by a human,
    and it is the same behaviour for both - there is no script mode and no
    autodetection. A reader synchronises like this:

      - read until the stream ends with CMD_PARSER_ACK or CMD_PARSER_NAK. That
        is the whole rule - one comparison, no line splitting, no assumption
        about what the output contained.
      - ACK means the command reported no failure, NAK means it did. The
        human-readable reason stays where it was, in the command's own output.
      - send the next line only then. That is also the flow control: with one
        command in flight the receive buffer cannot overrun, whatever its size
        and however long the transmit path blocked.
      - call cmd_parser_prompt() once at startup so a reader can synchronise
        before sending anything at all.

    Do NOT try to recognise readiness from the printable prompt. It cannot be
    done: command output may carry arbitrary user-supplied text, so both "the
    stream ends with '> '" and the stricter "the bytes after the last newline are
    '> '" produce false positives. "help" prints "calc <a> <op> <b>", and
    "upper > abc" prints a whole line starting with "> ". Both were observed
    failing before the control byte was introduced.

    One limit worth knowing: a command handler must end its output with a
    newline, otherwise the prompt is appended to a half-finished line. That no
    longer breaks a machine reader, but it looks wrong to a human.

      - Registration is a plain array filled by an explicit cmd_register()
        call (no custom linker section - the MCC/Harmony-generated linker
        scripts are regenerated on every MCC run and must not be hand-edited).
      - Input is fed one byte at a time via cmd_parser_feed_char(), so it
        fits a polling task instead of a dedicated blocking-read thread -
        the same shape jkb_dev.c already uses with
        SERCOM0_USART_ReadCountGet()/SERCOM0_USART_Read() from a task that
        calls JKB_DEV_Tasks() every 100 ms (see src/config/default/tasks.c).
      - Output goes through a caller-supplied write function (e.g. uartWrite()
        from app.c) instead of a fixed printk()/console_putchar() path, so
        this module isn't tied to one specific UART.

    Usage (mirrors led_ctrl.c in the reference project):

      static void led_cmd(int argc, char **argv)
      {
          if (argc == 2 && strcmp(argv[1], "on") == 0) { ... }
          else { cmd_parser_write("usage: led on|off\r\n"); }
      }
      CMD_DEFINE(led, "led", led_cmd, "led on|off - LED control");

      void APP_LED_Initialize(void)
      {
          ...
          cmd_register(&cmd_led);
      }

    Wiring into an existing polled UART reader (e.g. next to jkb_dev.c),
    once per received byte:

      cmd_parser_init(uartWrite);
      ...
      while (SERCOM0_USART_ReadCountGet() > 0)
      {
          uint8_t byte;
          SERCOM0_USART_Read(&byte, 1);
          cmd_parser_feed_char((char)byte);
      }

 *******************************************************************************/

#ifndef _CMD_PARSER_H
#define _CMD_PARSER_H

// *****************************************************************************
// Section: Included Files
// *****************************************************************************

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// *****************************************************************************
// Section: Configuration
// *****************************************************************************

/* Deepest command in practice is 3 tokens ("led blink 500"); 12 leaves room
   for multi-byte peripheral transfers (e.g. "i2c write 0x50 b0 b1 b2 b3 b4")
   without growing the per-line stack frame much - it's an array of pointers
   on the dispatcher's stack frame, not static RAM. */
#define CMD_PARSER_MAX_ARGS      12
#define CMD_PARSER_MAX_COMMANDS  16
#define CMD_PARSER_LINE_MAX_LEN  64

/* History is a single packed byte pool, not one fixed-size slot per line: each
   entry costs its own length plus one length byte, so short commands (the
   normal case) cost far less than CMD_PARSER_LINE_MAX_LEN. BYTES caps the RAM,
   DEPTH caps how many lines are kept - whichever runs out first evicts the
   oldest entry. 128 B holds roughly ten typical commands. */
#define CMD_PARSER_HISTORY_BYTES 128
#define CMD_PARSER_HISTORY_DEPTH   8

/* Readiness token, emitted after every submitted line - see the protocol
   contract in the file header. Identical for human and machine callers; there is
   deliberately no separate script mode. */
#define CMD_PARSER_PROMPT "> "

/* The prompt is followed by one reserved control byte, which is what makes
   readiness unambiguous for a machine: ACK when the command succeeded, NAK when
   it failed. A terminal ignores both, so a human sees nothing but the prompt.

   A printable prompt cannot be recognised reliably by pattern matching, because
   command output may contain arbitrary user-supplied text at any position -
   "upper > abc" prints a line that starts with "> ". Only a byte the payload
   cannot contain works, which is the same reason SLIP stuffs its delimiter and
   FTP prefixes every line.

   0x11/0x13 are deliberately not used: XON/XOFF software flow control in a
   terminal or driver would consume them. */
#define CMD_PARSER_ACK "\x06" /* ready, previous command reported no failure */
#define CMD_PARSER_NAK "\x15" /* ready, previous command reported a failure */

/* Line/cursor positions are held in uint8_t to keep the module's RAM down. */
#if CMD_PARSER_LINE_MAX_LEN > 255
#error "CMD_PARSER_LINE_MAX_LEN must be <= 255 (positions are stored in uint8_t)"
#endif
#if CMD_PARSER_HISTORY_BYTES > 255
#error "CMD_PARSER_HISTORY_BYTES must be <= 255 (pool offsets are stored in uint8_t)"
#endif

// *****************************************************************************
// Section: Data Types
// *****************************************************************************

/* Handler for one command. argv[0] is the command name; argv[1..argc-1] are
   its arguments. Handlers print their own output/usage via
   cmd_parser_write(), hence void. */
typedef void (*cmd_fn_t)(int argc, char **argv);

typedef struct
{
    const char *name; /* first token to match, e.g. "led" */
    cmd_fn_t fn;       /* dispatched handler */
    const char *help;  /* one-line help, printed by the "help" command */
} cmd_t;

/* Declares a static const cmd_t named cmd_<id>. The caller still has to hand
   it to cmd_register() once (typically from the owning module's own init
   function, alongside its other one-time setup) - registration is explicit
   here, not automatic, so no linker/startup mechanism has to be trusted.
   `id` must be unique within its source file (it forms the variable name). */
#define CMD_DEFINE(id, name_, fn_, help_) \
    static const cmd_t cmd_##id = { .name = (name_), .fn = (fn_), .help = (help_) }

/* Output sink. Takes as many of the `len` bytes as it can and returns how many
   it actually accepted; 0 means "full, nothing taken". `data` is not null
   terminated - only `len` bytes may be read.

   This is deliberately the same contract Harmony's ring-buffer PLIB already
   uses (SERCOMx_USART_Write() returns the number of bytes queued), so a UART
   write can be handed over almost directly. */
typedef size_t (*cmd_parser_write_fn_t)(const char *data, size_t len);

/* Optional. Called by cmd_parser_write() whenever the sink reported "full", so
   the caller can let it drain - typically vTaskDelay(1) under FreeRTOS, or a
   poll of the transmit engine on a bare-metal build.

   With a yield function installed, output is loss-free: the parser keeps
   retrying until every byte has been accepted. Without one it falls back to
   "write what fits, drop the rest", which is all a fast sink such as a PC's
   stdout ever needs. */
typedef void (*cmd_parser_yield_fn_t)(void);

// *****************************************************************************
// Section: Interface Functions
// *****************************************************************************

/* One-time setup. writeFn must stay valid for the module's lifetime. Also
   registers the built-in "help" command and clears any yield function, so
   cmd_parser_set_yield() has to be called after this, not before. */
void cmd_parser_init(cmd_parser_write_fn_t writeFn);

/* Installs the drain hook described at cmd_parser_yield_fn_t; pass NULL to
   remove it. Without it, output that does not fit into the sink is dropped -
   which at 115200 baud already happens on the built-in "help" (its reply is
   larger than a typical 512-byte transmit buffer). */
void cmd_parser_set_yield(cmd_parser_yield_fn_t yieldFn);

/* Emits the prompt plus ACK once. Call this after cmd_parser_init() and
   cmd_parser_set_yield(), as soon as the output path is usable, so that a reader
   has a synchronisation point before it sends its first command. Afterwards the
   module emits prompt and status on its own after every submitted line. */
void cmd_parser_prompt(void);

/* Marks the command currently running as failed, so the readiness marker after
   it is NAK instead of ACK. Call it from a command handler, in addition to
   whatever it prints for a human - the handler's own message stays the
   human-readable explanation, this only makes the outcome machine-readable.
   The flag is cleared before each command, so it never leaks into the next one.

   Usage errors and unparsable arguments are failures; a well-formed query with a
   negative answer ("not found") is not. */
void cmd_parser_fail(void);

/* Aborting a long-running command
   -------------------------------
   A command with a lot of output blocks for a long time: at 115200 baud a 64 KiB
   hex dump is 312 KiB of text and takes 27 seconds, during which the parser sits
   inside cmd_parser_write() and reads nothing. Without a way out, neither a human
   pressing Ctrl+C nor a script can stop it.

   cmd_parser_abort() is therefore called from OUTSIDE the parser - typically from
   the yield function, which runs while the transmit path is blocked and can peek
   at the receive buffer for an abort byte. It must not feed the parser: calling
   cmd_parser_feed_char() from inside a write would re-enter the line editor and
   dispatch while an earlier command is still printing.

   A handler with long output polls cmd_parser_aborted() between chunks and
   returns early. An aborted command is reported as NAK, so a reader knows the
   output is incomplete. Both flags are cleared before each command. */
void cmd_parser_abort(void);
bool cmd_parser_aborted(void);

/* Adds a command to the registry. Returns false if CMD_PARSER_MAX_COMMANDS
   is already exhausted (raise the define instead of ignoring this). */
bool cmd_register(const cmd_t *cmd);

/* Feed one received byte. Call this for every byte read from the UART/stream
   (e.g. inside the same loop that currently calls SERCOM0_USART_Read() in
   jkb_dev.c). Handles echo, backspace/delete, Up/Down history recall, and -
   on Enter - tokenizes the line and dispatches it to the matching registered
   command. */
void cmd_parser_feed_char(char c);

/* Convenience wrapper around the writeFn passed to cmd_parser_init(), so
   command handlers don't need to keep their own reference to it. */
void cmd_parser_write(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* _CMD_PARSER_H */

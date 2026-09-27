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
    cmd_parser.c

  Summary:
    See cmd_parser.h for the design and how this differs from the Zephyr
    original (https://github.com/zabooh/pic32cm-pl10-cnano-zephyr) it was
    ported from.

  Description:
    Generic front-end: a hand-rolled line editor (with bash-style Up/Down
    history) over cmd_parser_feed_char(), plus a tokenizer that dispatches
    each completed line to the command registry. This module knows nothing
    about led/adc/etc. - every command lives in its owning module and
    registers itself via CMD_DEFINE()+cmd_register(); adding a command never
    touches this file. Only "help" is built in here, since it is intrinsic
    to the registry.

 *******************************************************************************/

// *****************************************************************************
// Section: Included Files
// *****************************************************************************

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "cmd_parser.h"

// *****************************************************************************
// Section: Local Constants
// *****************************************************************************

#define KEY_BS    0x08
#define KEY_DEL   0x7F
#define KEY_ESC   0x1B
#define KEY_CSI   '['
#define KEY_SS3   'O'
#define KEY_TILDE '~'

/* Final bytes of the CSI sequences this module understands. */
#define KEY_UP    'A'
#define KEY_DOWN  'B'
#define KEY_RIGHT 'C'
#define KEY_LEFT  'D'
#define KEY_END   'F'
#define KEY_HOME  'H'

/* Numeric CSI parameters, i.e. ESC '[' <n> '~'. */
#define CSI_NUM_HOME    1U
#define CSI_NUM_DELETE  3U
#define CSI_NUM_END     4U

/* Control characters used as readline-style editing shortcuts. */
#define KEY_CTRL_A 0x01 /* start of line */
#define KEY_CTRL_E 0x05 /* end of line */
#define KEY_CTRL_K 0x0B /* kill to end of line */
#define KEY_CTRL_U 0x15 /* kill whole line */
#define KEY_CTRL_W 0x17 /* delete word left */

/* Escape parser states. */
#define ESC_IDLE   0U
#define ESC_GOT_ESC 1U
#define ESC_GOT_CSI 2U

// *****************************************************************************
// Section: Local Data
// *****************************************************************************

static const cmd_t *commands[CMD_PARSER_MAX_COMMANDS];
static uint8_t commandCount;

static cmd_parser_write_fn_t writeFn;
static cmd_parser_yield_fn_t yieldFn;

/* Outcome of the command currently running: decides ACK vs NAK, and lets a
   handler with long output notice that it should stop. Packed into one byte
   rather than two bools - this module counts RAM. */
#define CMD_FLAG_FAILED  0x01U
#define CMD_FLAG_ABORTED 0x02U
static uint8_t commandFlags;

/* Submitted lines, packed back-to-back into one pool, oldest first. Each entry
   is a single length byte followed by that many characters and no terminator -
   so a 6-character command costs 7 bytes instead of a full
   CMD_PARSER_LINE_MAX_LEN slot. Entries always start at offset 0 and never
   wrap: evicting the oldest one memmove()s the rest down, which keeps indexed
   access trivial and costs far less code than a true ring buffer. */
static char historyPool[CMD_PARSER_HISTORY_BYTES];
static uint8_t historyUsed;  /* bytes of historyPool in use */
static uint8_t historyCount; /* number of entries currently stored */

/* Line editor state, persists across cmd_parser_feed_char() calls. */
static char lineBuf[CMD_PARSER_LINE_MAX_LEN];
static uint8_t lineLen;
static uint8_t cursorPos;  /* 0..lineLen, insertion point inside lineBuf */
static uint8_t historyPos; /* == historyCount means "not browsing" */
static uint8_t escState;   /* ESC_IDLE / ESC_GOT_ESC / ESC_GOT_CSI */
static uint8_t escParam;   /* numeric CSI parameter collected so far */
static uint8_t lastChar;   /* last byte seen, to swallow a trailing '\n' after '\r' */

// *****************************************************************************
// Section: Local Functions
// *****************************************************************************

static void echoChar(char c)
{
    char s[2] = { c, '\0' };
    cmd_parser_write(s);
}

static void echoBackspace(void)
{
    cmd_parser_write("\b \b");
}

/* Move the cursor `n` columns left. A bare backspace only moves, it does not
   erase - that is what makes ANSI cursor positioning unnecessary here. */
static void moveLeft(uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++)
    {
        cmd_parser_write("\b");
    }
}

/* Park the cursor at the end of the line by re-echoing what it passes over. */
static void moveToEnd(void)
{
    while (cursorPos < lineLen)
    {
        echoChar(lineBuf[cursorPos]);
        cursorPos++;
    }
}

/* The single redraw primitive behind every editing operation: reprint the line
   from `from` to its end, blank out `stale` columns that the previous, longer
   line left behind, then walk the cursor back to cursorPos.

   Inserting uses stale = 0, deleting one character uses stale = 1, the kill
   shortcuts pass however many characters they removed. With the cursor at the
   end of the line the loops collapse to exactly the output the plain
   append/backspace path produced before line editing existed. */
static void refreshTail(uint8_t from, uint8_t stale)
{
    uint8_t i;

    for (i = from; i < lineLen; i++)
    {
        echoChar(lineBuf[i]);
    }
    for (i = 0; i < stale; i++)
    {
        cmd_parser_write(" ");
    }
    moveLeft((uint8_t)((lineLen + stale) - cursorPos));
}

/* Erase `len` already-echoed characters left of the cursor and reprint
   `text` (`lenNew` chars) in their place - used for history recall, which
   replaces the whole line at once. The caller must have the cursor at the end
   of the old line (see moveToEnd()). */
static void redrawLine(uint8_t len, const char *text, uint8_t lenNew)
{
    uint8_t i;

    for (i = 0; i < len; i++)
    {
        echoBackspace();
    }
    for (i = 0; i < lenNew; i++)
    {
        echoChar(text[i]);
    }
}

/* Remove `count` characters starting at `at`, then repaint. */
static void deleteRange(uint8_t at, uint8_t count)
{
    if (count == 0U)
    {
        return;
    }

    (void)memmove(&lineBuf[at], &lineBuf[at + count],
                  (size_t)(lineLen - (at + count)));
    lineLen = (uint8_t)(lineLen - count);
    cursorPos = at;
    lineBuf[lineLen] = '\0';

    refreshTail(at, count);
}

/* Locate entry `pos` (0 = oldest kept, historyCount - 1 = newest) in the packed
   pool. Returns a pointer to its characters, which are NOT null terminated, and
   writes the length to *len. `pos` must be < historyCount. */
static const char *historyEntry(uint8_t pos, uint8_t *len)
{
    uint8_t offset = 0;
    uint8_t i;

    for (i = 0; i < pos; i++)
    {
        offset = (uint8_t)(offset + 1U + (uint8_t)historyPool[offset]);
    }

    *len = (uint8_t)historyPool[offset];
    return &historyPool[offset + 1U];
}

/* Discard the oldest entry and slide the remainder down to offset 0. */
static void historyDropOldest(void)
{
    uint8_t size;

    if (historyCount == 0U)
    {
        return;
    }

    size = (uint8_t)(1U + (uint8_t)historyPool[0]);
    historyUsed = (uint8_t)(historyUsed - size);
    (void)memmove(&historyPool[0], &historyPool[size], (size_t)historyUsed);
    historyCount--;
}

static void historyAdd(const char *line, uint8_t len)
{
    uint8_t need = (uint8_t)(len + 1U);

    if (len == 0U)
    {
        return; /* empty lines are never remembered */
    }

    /* Repeating the previous command must not evict older ones - with a pool
       this small, duplicates are expensive. */
    if (historyCount > 0U)
    {
        uint8_t previousLen;
        const char *previous = historyEntry((uint8_t)(historyCount - 1U), &previousLen);

        if ((previousLen == len) && (memcmp(previous, line, (size_t)len) == 0))
        {
            return;
        }
    }

    if (need > CMD_PARSER_HISTORY_BYTES)
    {
        return; /* a single line longer than the whole pool cannot be kept */
    }

    while ((historyCount >= CMD_PARSER_HISTORY_DEPTH) ||
           ((uint8_t)(CMD_PARSER_HISTORY_BYTES - historyUsed) < need))
    {
        historyDropOldest();
    }

    historyPool[historyUsed] = (char)len;
    (void)memcpy(&historyPool[historyUsed + 1U], line, (size_t)len);
    historyUsed = (uint8_t)(historyUsed + need);
    historyCount++;
}

/* Replace the whole line with history entry `pos`, or with an empty line when
   pos == historyCount ("browsed past the newest entry"). */
static void historyRecall(uint8_t pos)
{
    uint8_t oldLen;

    /* redrawLine() erases by counting columns back from the line end, so the
       cursor has to be there first - it may be anywhere after editing. */
    moveToEnd();
    oldLen = lineLen;

    if (pos == historyCount)
    {
        lineLen = 0;
    }
    else
    {
        uint8_t len;
        const char *entry = historyEntry(pos, &len);

        lineLen = len;
        (void)memcpy(lineBuf, entry, (size_t)len);
    }

    lineBuf[lineLen] = '\0';
    cursorPos = lineLen;
    redrawLine(oldLen, lineBuf, lineLen);
}

/* Act on the final byte of an ESC '[' (CSI) or ESC 'O' (SS3) sequence; escParam
   holds the numeric parameter collected before it (0 when there was none).
   Unknown sequences are ignored rather than inserted as text - which is what
   makes F1..F4 (SS3 'P'..'S') harmless here. */
static void handleCsiFinal(uint8_t byte)
{
    switch (byte)
    {
        case KEY_UP:
            if (historyPos > 0U)
            {
                historyPos--;
                historyRecall(historyPos);
            }
            break;

        case KEY_DOWN:
            if (historyPos < historyCount)
            {
                historyPos++;
                historyRecall(historyPos);
            }
            break;

        case KEY_LEFT:
            if (cursorPos > 0U)
            {
                cursorPos--;
                moveLeft(1U);
            }
            break;

        case KEY_RIGHT:
            if (cursorPos < lineLen)
            {
                echoChar(lineBuf[cursorPos]);
                cursorPos++;
            }
            break;

        case KEY_HOME:
            moveLeft(cursorPos);
            cursorPos = 0;
            break;

        case KEY_END:
            moveToEnd();
            break;

        case KEY_TILDE:
            /* ESC '[' <n> '~' - Home/End/Delete on many terminals. */
            switch (escParam)
            {
                case CSI_NUM_HOME:
                    moveLeft(cursorPos);
                    cursorPos = 0;
                    break;

                case CSI_NUM_END:
                    moveToEnd();
                    break;

                case CSI_NUM_DELETE:
                    if (cursorPos < lineLen)
                    {
                        deleteRange(cursorPos, 1U);
                    }
                    break;

                default:
                    break;
            }
            break;

        default:
            break;
    }
}

/* Split `line` into argv[] in place (spaces overwritten with '\0'). Returns
   argc; extra tokens beyond `max` are left attached to the last argv entry. */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;

    while (*line != '\0' && argc < max)
    {
        while (*line == ' ')
        {
            *line++ = '\0';
        }
        if (*line == '\0')
        {
            break;
        }
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ')
        {
            line++;
        }
    }
    return argc;
}

static void dispatchLine(char *line)
{
    char *argv[CMD_PARSER_MAX_ARGS];
    int argc = tokenize(line, argv, CMD_PARSER_MAX_ARGS);
    uint8_t i;

    if (argc == 0)
    {
        return; /* empty line */
    }

    for (i = 0; i < commandCount; i++)
    {
        if (strcmp(argv[0], commands[i]->name) == 0)
        {
            commands[i]->fn(argc, argv);
            return;
        }
    }
    cmd_parser_write("unknown command: ");
    cmd_parser_write(argv[0]);
    cmd_parser_write("\r\n");
    cmd_parser_fail();
}

/* Built-in "help": lists every registered command's one-line help.
   Registration order is call order (not sorted). */
static void helpCmd(int argc, char **argv)
{
    uint8_t i;

    (void)argc;
    (void)argv;

    cmd_parser_write("Available commands:\r\n");
    for (i = 0; i < commandCount; i++)
    {
        cmd_parser_write("  ");
        cmd_parser_write(commands[i]->help);
        cmd_parser_write("\r\n");
    }
}
CMD_DEFINE(help, "help", helpCmd, "help - show this help");

// *****************************************************************************
// Section: Interface Functions
// *****************************************************************************

/* The module's only flow control, and the only place that knows about it.
   The sink reports how many bytes it took; whatever is left over is retried
   after giving the caller a chance to let it drain.

   This matters at real baud rates: 522 bytes of "help" output need 45 ms of
   line time at 115200, far more than any sensible transmit buffer holds. A
   bigger buffer only moves the threshold.

   Note which half does what, because it is easy to get backwards: the *retry
   loop* is what prevents loss, the yield function only makes the waiting
   cooperative. The PC harness proves it - its yield function is empty, so
   nothing ever waits, yet a sink that accepts one byte per call still receives
   every byte. Conversely a yield function without the retry loop would do
   nothing at all.

   The effect reaches further than this function: cmd_parser_write() does not
   return until the bytes are gone, so a command handler printing a long reply
   is itself throttled to line speed. That is why no buffer is needed here - the
   "buffer" is the handler's suspended execution.

   Without a yield function there is nobody to wait for, so the remainder is
   dropped rather than spun on forever. */
void cmd_parser_write(const char *str)
{
    size_t len;
    size_t sent = 0;

    if (writeFn == NULL)
    {
        return;
    }

    len = strlen(str);

    while (sent < len)
    {
        sent += writeFn(str + sent, len - sent);

        if (sent < len)
        {
            if (yieldFn == NULL)
            {
                return;
            }
            yieldFn();
        }
    }
}

void cmd_parser_init(cmd_parser_write_fn_t writeFunction)
{
    writeFn = writeFunction;
    yieldFn = NULL; /* opt in afterwards via cmd_parser_set_yield() */
    commandFlags = 0;
    commandCount = 0;
    lineLen = 0;
    cursorPos = 0;
    historyPos = 0;
    escState = ESC_IDLE;
    escParam = 0;
    lastChar = 0;
    historyCount = 0;
    historyUsed = 0;

    (void)cmd_register(&cmd_help);
}

void cmd_parser_set_yield(cmd_parser_yield_fn_t yieldFunction)
{
    yieldFn = yieldFunction;
}

/* Prompt plus the reserved status byte. The byte is what a machine matches on -
   the printable prompt cannot be recognised reliably, because command output may
   contain it (see the header). */
static void emitReady(bool failed)
{
    cmd_parser_write(CMD_PARSER_PROMPT);
    cmd_parser_write(failed ? CMD_PARSER_NAK : CMD_PARSER_ACK);
}

void cmd_parser_prompt(void)
{
    emitReady(false);
}

void cmd_parser_fail(void)
{
    commandFlags |= CMD_FLAG_FAILED;
}

void cmd_parser_abort(void)
{
    /* An aborted command did not run to completion, so it is also a failure -
       that way a reader sees NAK and knows the output is incomplete. */
    commandFlags |= (CMD_FLAG_ABORTED | CMD_FLAG_FAILED);
}

bool cmd_parser_aborted(void)
{
    return (commandFlags & CMD_FLAG_ABORTED) != 0U;
}

bool cmd_register(const cmd_t *cmd)
{
    if (commandCount >= CMD_PARSER_MAX_COMMANDS)
    {
        return false;
    }
    commands[commandCount++] = cmd;
    return true;
}

void cmd_parser_feed_char(char c)
{
    uint8_t byte = (uint8_t)c;

    /* A "\r\n" pair can arrive as two separate feed_char() calls; swallow
       the trailing '\n' after a '\r' so it isn't seen as an Enter on an
       empty line. */
    if (byte == (uint8_t)'\n' && lastChar == (uint8_t)'\r')
    {
        lastChar = byte;
        return;
    }
    lastChar = byte;

    if (escState == ESC_GOT_ESC)
    {
        /* Two introducers lead into a sequence, not one. Besides CSI (ESC '[')
           terminals also use SS3 (ESC 'O'): in application cursor key mode the
           arrows arrive as ESC 'O' A..D, and F1..F4 are ESC 'O' P..S on most
           terminals - the latter needs no mode change at all. Treating only
           '[' as an introducer dropped the 'O' and then handed the final byte
           to the normal input path, so pressing F1 inserted a stray 'P' into
           the command line. Both sequences share the same final-byte handling:
           it acts on A..D/F/H and discards anything else. */
        escState = ((byte == (uint8_t)KEY_CSI) || (byte == (uint8_t)KEY_SS3))
                       ? ESC_GOT_CSI
                       : ESC_IDLE;
        escParam = 0;
        return;
    }

    if (escState == ESC_GOT_CSI)
    {
        /* Collect a numeric parameter; the sequence ends on the first byte that
           is neither a digit nor a separator, which handleCsiFinal() then
           interprets.

           ';' has to be swallowed here as well: terminals send modified arrows
           as ESC '[' 1 ';' 5 'D' (Ctrl+Left and friends). Without this the
           sequence would break at the ';' and the remaining "5D" would be
           inserted into the line as text. Only the last parameter is kept,
           which is enough to treat a modified arrow like its plain form. */
        if ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'9'))
        {
            escParam = (uint8_t)((escParam * 10U) + (byte - (uint8_t)'0'));
            return;
        }
        if (byte == (uint8_t)';')
        {
            escParam = 0;
            return;
        }

        escState = ESC_IDLE;
        handleCsiFinal(byte);
        return;
    }

    if (byte == (uint8_t)KEY_ESC)
    {
        escState = ESC_GOT_ESC;
        escParam = 0;
        return;
    }

    if (byte == (uint8_t)'\r' || byte == (uint8_t)'\n')
    {
        moveToEnd(); /* submitting from mid-line must not swallow the tail */
        cmd_parser_write("\r\n");
        lineBuf[lineLen] = '\0';
        historyAdd(lineBuf, lineLen);

        commandFlags = 0; /* never let a verdict leak into the next command */
        dispatchLine(lineBuf); /* tokenizes lineBuf in place - history first */

        lineLen = 0;
        cursorPos = 0;
        historyPos = historyCount;

        /* Strictly last: prompt plus status mean "processed and ready". Because
           cmd_parser_write() only returns once the sink accepted the bytes, a
           reader seeing the status byte knows the command's whole output is out.
           An empty line gets one too - "ready again" is the truth there as
           well. The '\n' of a "\r\n" pair returned early further up, so it
           cannot produce a second prompt. */
        emitReady((commandFlags & CMD_FLAG_FAILED) != 0U);
        return;
    }

    if (byte == (uint8_t)KEY_BS || byte == (uint8_t)KEY_DEL)
    {
        if (cursorPos > 0U)
        {
            moveLeft(1U);
            deleteRange((uint8_t)(cursorPos - 1U), 1U);
        }
        return;
    }

    if (byte == (uint8_t)KEY_CTRL_A)
    {
        moveLeft(cursorPos);
        cursorPos = 0;
        return;
    }

    if (byte == (uint8_t)KEY_CTRL_E)
    {
        moveToEnd();
        return;
    }

    if (byte == (uint8_t)KEY_CTRL_U)
    {
        moveLeft(cursorPos);
        cursorPos = 0;
        deleteRange(0U, lineLen);
        return;
    }

    if (byte == (uint8_t)KEY_CTRL_K)
    {
        deleteRange(cursorPos, (uint8_t)(lineLen - cursorPos));
        return;
    }

    if (byte == (uint8_t)KEY_CTRL_W)
    {
        uint8_t start = cursorPos;

        /* Skip the run of spaces left of the cursor, then the word itself. */
        while ((start > 0U) && (lineBuf[start - 1U] == ' '))
        {
            start--;
        }
        while ((start > 0U) && (lineBuf[start - 1U] != ' '))
        {
            start--;
        }

        if (start < cursorPos)
        {
            uint8_t count = (uint8_t)(cursorPos - start);

            moveLeft(count);
            deleteRange(start, count);
        }
        return;
    }

    if ((isprint((int)byte) != 0) && ((lineLen + 1U) < CMD_PARSER_LINE_MAX_LEN))
    {
        (void)memmove(&lineBuf[cursorPos + 1U], &lineBuf[cursorPos],
                      (size_t)(lineLen - cursorPos));
        lineBuf[cursorPos] = (char)byte;
        lineLen++;
        cursorPos++;
        lineBuf[lineLen] = '\0';

        /* With the cursor at the end this emits exactly the one echoed
           character the append-only path produced before. */
        refreshTail((uint8_t)(cursorPos - 1U), 0U);
    }
}

/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

/*
 * console.h - the console of the ADC/DMA example (cli.c)
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* size_t, for console_write_raw() */

/* UART2 up on the FRC, before the clocks are touched. From here on
 * console_puts() works. */
void console_early_init(void);
/* After clock_init(): baud generator on the PLL clock, parser, banner,
 * receive interrupt. */
void cli_init(void);
/* The lab's commands, registered by cli_init() after the core's: strong in
 * cli_lab.c, an empty weak default in cli.c for a build without the lab
 * (CORE.3, 02.10.2026). */
void cli_register_lab(void);
/* The application's part of the "sigproc" command (02.10.2026, sigproc.h's
 * application hooks): cmd_sigproc_fn() hands every sub-command it does not
 * know to sigproc_app_cmd() - 0: not the application's (cli.c prints the
 * usage), 1: done, -1: refused, the application has said why - and
 * appends sigproc_app_status() to its status. Weak and empty in cli.c. */
int  sigproc_app_cmd(int argc, char **argv);
void sigproc_app_status(void);
/* Baud generator re-matched to the current CPU clock; used by fail(). */
void console_sync_baud(void);
/* Re-establish pins, PPS and UART from scratch, assuming nothing about
 * the current state. Used by the trap handler, which cannot rely on the
 * console still being intact. */
void console_force_up(void);
/* Bytes received while capture_service() ran with the signal processing
 * on were held back (uart_rx_hook(), cli.c); this hands them to the parser
 * - called by main()'s loop after every capture_service(), returns at
 * once when nothing is held back. */
void console_rx_resume(void);
/* A measuring command's output polled, finished before it measures on
 * (cli.c says why): begin returns what end needs to put back. */
bool console_quiet_begin(void);
void console_quiet_end(bool was);
/* Wait (bounded) until the transmitter is empty, shift register included.
 * console_puts() returns as soon as the last character is in the FIFO,
 * so anything that changes the clock or the baud generator right after
 * a message must call this first, or the tail of the message goes out
 * at the wrong rate. */
void console_flush(void);

/* Send bytes that are not a C string. cmd_parser_write() takes a
 * NUL-terminated string and therefore cannot carry a 0x00 byte, which
 * every block of samples is full of. Same transmit FIFO loop as the
 * parser's own sink, with the same Ctrl+C check while it drains: a block
 * that is aborted simply ends early, the CRC line and the prompt still
 * come, and the client reports a short read rather than hanging.
 * Returns how many bytes went out. */
size_t console_write_raw(const uint8_t *data, size_t len);
/* Blocking trace output, safe from main() and from fail(). */
void console_puts(const char *s);
void console_kv(const char *key, uint32_t v);        /* "key: 123"        */
void console_kv_hex(const char *key, uint32_t v);    /* "key: 0x00000123" */
/* The same three, but only with BOOT_VERBOSE 1 (board.h): the start-up
 * steps' running commentary. Empty otherwise. */
void console_trace(const char *s);
void console_trace_kv(const char *key, uint32_t v);
void console_trace_kv_hex(const char *key, uint32_t v);
/* One line with every counter, for the periodic trace from main(). */
void console_status_line(void);
/* "[half] n=.. min=.. max=.. mean=.. pp=.." of the completed half: the
 * same figures as the "stats" command, after every [stat] line. */
void console_half_stats(void);
/* UART, its interrupt and its pin routing as "name: 0x........" lines
 * (part of regs_dump()). */
void console_regs_dump(void);

#endif /* CONSOLE_H */

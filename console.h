/*
 * console.h - the console of the ADC/DMA example (cli.c)
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

/* UART2 up on the FRC, before the clocks are touched. From here on
 * console_puts() works. */
void console_early_init(void);
/* After clock_init(): baud generator on the PLL clock, parser, banner,
 * receive interrupt. */
void cli_init(void);
/* Baud generator re-matched to the current CPU clock; used by fail(). */
void console_sync_baud(void);
/* Re-establish pins, PPS and UART from scratch, assuming nothing about
 * the current state. Used by the trap handler, which cannot rely on the
 * console still being intact. */
void console_force_up(void);
/* Wait (bounded) until the transmitter is empty, shift register included.
 * console_puts() returns as soon as the last character is in the FIFO,
 * so anything that changes the clock or the baud generator right after
 * a message must call this first, or the tail of the message goes out
 * at the wrong rate. */
void console_flush(void);
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
/* The rate sweep (the "sweep" command): overrun vs sample rate from
 * 1.25 to 40 MSPS, `halves` buffer halves per point, one line per rate.
 * Blocking, takes a few seconds, restores sample time and run state. */
/* choose: after the table, take the highest rate whose "process" run had
 * overrun 0 and missed 0 (the boot does); false restores the previous
 * period (the "sweep" command). */
void console_sweep(uint32_t halves, bool choose);
/* UART, its interrupt and its pin routing as "name: 0x........" lines
 * (part of regs_dump()). */
void console_regs_dump(void);

#endif /* CONSOLE_H */

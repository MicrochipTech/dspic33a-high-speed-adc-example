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
/* Blocking trace output, safe from main() and from fail(). */
void console_puts(const char *s);
void console_kv(const char *key, uint32_t v);        /* "key: 123"        */
void console_kv_hex(const char *key, uint32_t v);    /* "key: 0x00000123" */
/* One line with every counter, for the periodic trace from main(). */
void console_status_line(void);
/* UART, its interrupt and its pin routing as "name: 0x........" lines
 * (part of regs_dump()). */
void console_regs_dump(void);

#endif /* CONSOLE_H */

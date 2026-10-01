/*
 * uart.h - UART2 transport for the console (uart.c), P5.1/P5.2 of
 * docs/IMPLEMENTATION-PLAN.md (27.09.2026), V4 of docs/REFACTORING-PROPOSAL.md
 *
 * Transport on the EV74H48A
 *   UART2 on the MCP2221A USB-UART channel: its TX pin drives the board's
 *   RX pin, and vice versa (pins in board.h, the CONSOLE_TX_.. /
 *   CONSOLE_RX_.. macros), 115200 8N1. The MCP2221A implements standard USB CDC and shows up on
 *   the PC as its own COM port (user guide DS70005562D 2.1.1). The PPS
 *   code is the one Microchip's own example uses on this board (Table
 *   "Output Selection for Remappable Pins", p613). The board's other
 *   USB-UART channel (the PKOB4's) is not used by this console. The
 *   EV17P63A Curiosity Nano (board.h, BOARD=2) wires the same UART unit
 *   to the on-board debugger's CDC channel instead, on different pins.
 *
 * What this driver knows, and what it does not
 *   uart.c owns every UART2/PPS/interrupt-controller register the console
 *   uses and nothing else: it has no notion of "115200 baud", of the FRC
 *   vs. PLL2 clock switch, or of the command parser. The baud-rate divisor
 *   is always the caller's decision (cli.c works out which of two values
 *   fits the clock the CPU happens to be on right now, from clock.c) and
 *   arrives as a plain number; the receive interrupt hands each byte to
 *   uart_rx_hook(), a weak symbol with an empty default, and cli.c's
 *   strong override is the only place that knows about the parser and the
 *   receive diagnostics counters. This mirrors the other drivers' hook
 *   pattern (clock.c's clock_fail_hook(), clock.h) except that the strong
 *   override lives in cli.c rather than in port_impl.c: what a received
 *   byte means is the console's business specifically, not a generic
 *   "the application" concern the way a clock failure is.
 *
 *   Unlike adc.c/dma.c/clock.c/dac.c/sccp.c, uart.c does not report
 *   through port/log.h and does not wait through port/wait.h: it has
 *   nothing to log (it IS the transport console_puts()/console_kv() end
 *   up writing through - going through port_log() here would call back
 *   into itself) and none of its polling loops stop with a panic (see
 *   UART_TX_WAIT_LIMIT below - "better a garbled line than none", not a
 *   stop). It still owns none of console.h, diag.h, capture.h or
 *   cmd_parser.h.
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "regs.h"       /* reg_visit_t (src/port, P4.8 pattern) */

typedef struct {
    uint32_t brg;   /* initial baud-rate-generator divisor (fractional
                     * mode, CLKMOD = 1): BRG = F_clk / baud, no -1 */
} uart_cfg_t;

/* How long to wait for the transmitter, in polling iterations. One
 * character takes 87 us at 115200 baud; on the 8 MHz FRC that is a few
 * hundred cycles, at 200 MHz about 17 000. 200 000 is far more than
 * either and still finite - which is the whole point: console_puts()
 * (cli.c) is called from _DefaultInterrupt() to report a trap, and if the
 * UART is not actually transmitting (wrong baud divider, ON bit cleared,
 * clock gone) an unbounded wait would silently swallow the one message
 * that explains the fault. Better a garbled line than none - the same
 * bound also drains the transmitter before a baud-rate or pin change
 * (uart_flush(), uart_reinit(), uart_set_baud() below). */
#define UART_TX_WAIT_LIMIT   200000u

/* Pins up (TRIS + PPS, board.h), UART2 registers from scratch, with the
 * given initial baud divisor. Used once, before the CPU clock changes
 * (was console_early_init()'s register part). */
void uart_init(const uart_cfg_t *cfg);

/* Re-establish pins, PPS and the UART from scratch, assuming nothing
 * about the current state, with the given baud divisor - safe to call
 * when the console is already up (it re-writes the same values) and from
 * interrupt context: it only waits through the bounded uart_flush()
 * (was console_force_up()). */
void uart_reinit(uint32_t brg);

/* Re-match the baud generator to `brg`, if it differs - a bounded
 * drain and one re-init, otherwise a no-op. Returns whether it actually
 * changed anything, so a caller can report it (was console_sync_baud(),
 * which did not report back; cli_init() needs to know to print its own
 * "reclocked" trace line only when true). */
bool uart_set_baud(uint32_t brg);

/* Non-blocking: writes as many of `len` bytes as fit in the transmit
 * FIFO right now, and returns how many that was (was console_write()'s
 * body - the parser's own output sink retries the remainder it is not
 * told went out; see cmd_parser.h, "Flow control"). */
size_t uart_write(const uint8_t *data, size_t len);

/* Wait, bounded (UART_TX_WAIT_LIMIT), until the transmitter - FIFO and
 * shift register - is empty; a no-op in the simulator build, which never
 * sets TXMTIF (was console_drain()/console_flush()). */
void uart_flush(void);

/* The raw status bits and raw register accesses cli.c's own send/receive
 * loops are built from - uart.c itself never branches on them, only
 * cli.c does, with three different waiting policies for three different
 * jobs: console_puts() polls uart_tx_full() with its own bound and
 * always writes in the end (never blocks forever, see
 * UART_TX_WAIT_LIMIT above); console_write_raw() polls it with a
 * cooperative yield instead, watching for Ctrl+C; that yield reads a
 * byte back with uart_rx_empty()/uart_getc() while it waits. */
bool    uart_tx_full(void);        /* TXBF: no room for one more byte  */
void    uart_putc(uint8_t b);      /* one byte into the FIFO, unchecked */
bool    uart_rx_empty(void);       /* RXBE: nothing received (yet)      */
uint8_t uart_getc(void);           /* one received byte, unchecked      */

/* Read the whole UART status register once and discard it - the "CPU
 * hammers the peripheral bus" load case of cli.c's "sweep" benchmark
 * (the console does exactly this while it prints); not used by uart.c
 * itself. */
void uart_stat_probe(void);

/* Receive interrupt (IRQ 102): priority as given, flag cleared, enabled.
 * Called once, after the transport and the application are both ready
 * (was cli_init()'s three lines at the end). */
void uart_enable_rx_irq(uint8_t priority);

/* Mask the receive interrupt, returning whether it was enabled; and put
 * that state back. For cli.c's console_rx_resume(), which feeds held-back
 * bytes to the parser from the main loop (since 01.10.2026). */
bool uart_rx_irq_mask(void);
void uart_rx_irq_restore(bool was);

/* Called once per received byte, from the receive interrupt
 * (_U2RXInterrupt, moved here from cli.c in P5.1), with the flag already
 * cleared for the byte that follows. Weak default does nothing; cli.c's
 * strong override feeds the command parser and keeps the receive
 * diagnostics counters - the receiver has no other use in this driver. */
void uart_rx_hook(uint8_t byte);

/* Register dump (part of diag.c's regs_dump(), by way of cli.c's
 * console_regs_dump() - see that function's comment for why it is not
 * called from diag.c directly). */
void uart_regs_visit(reg_visit_t visit);

#endif /* UART_H */

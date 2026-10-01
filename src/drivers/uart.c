/*
 * uart.c - UART2 transport for the console (uart.h; P5.1/P5.2,
 * 27.09.2026). Moved out of cli.c verbatim, register writes and their
 * datasheet comments unchanged, wrapped into the functions uart.h
 * declares - see uart.h for the pins/registers/hook design.
 */
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "board.h"
#include "uart.h"

/* ------------------------------------------------------------------ *
 * UART2 registers
 * ------------------------------------------------------------------ */

static void uart2_setup(uint32_t brg)
{
    U2CON = 0u;                       /* off while reconfiguring        */
    U2CONbits.CLKMOD = 1u;            /* fractional baud generator      */
    U2CONbits.CLKSEL = 0u;            /* standard speed peripheral clock*/
    U2CONbits.MODE   = 0u;            /* 8-bit, no parity               */
    U2CONbits.STP    = 0u;            /* one stop bit                   */
    U2BRG = brg;
    U2STAT = 0u;                      /* RXWM = 0: IRQ on one byte      */
    U2CONbits.ON   = 1u;
    U2CONbits.TXEN = 1u;
    U2CONbits.RXEN = 1u;
}

static void uart_pins_up(void)
{
    /* Pins: TX output, RX input (board.h). Peripheral pin select needs
     * IOLOCK cleared. A board whose RX line can float (board.h) gets
     * the internal pull-up first. */
#ifdef CONSOLE_RX_PULLUP
    CONSOLE_RX_PULLUP = 1u;
#endif
    CONSOLE_TX_TRIS  = 0u;
    CONSOLE_RX_TRIS  = 1u;
    RPCONbits.IOLOCK = 0u;
    CONSOLE_RX_RPINR = CONSOLE_RX_RP;
    CONSOLE_TX_RPOR  = CONSOLE_TX_FN;
    RPCONbits.IOLOCK = 1u;
}

void uart_init(const uart_cfg_t *cfg)
{
    uart_pins_up();
    uart2_setup(cfg->brg);
}

/* Bring the console back up from scratch, assuming nothing about the
 * current state of the pins, the PPS mapping or the UART.
 *
 * This is what _DefaultInterrupt() calls (through cli.c's
 * console_force_up()) before it reports a trap. A trap can have happened
 * anywhere, including inside clock_init() or after some other code
 * disturbed the peripheral, and in that situation uart_set_baud() is not
 * enough: it only fixes the baud divider. Here the routing is written
 * again as well, so the one message that explains the fault has the best
 * chance of getting out.
 *
 * Safe to call when the console is already up - it re-writes the same
 * values - and safe from interrupt context: no waiting except the
 * bounded uart_flush(). */
void uart_reinit(uint32_t brg)
{
    uart_flush();                      /* bounded; keep a partial line    */
    uart_pins_up();
    uart2_setup(brg);
}

bool uart_set_baud(uint32_t brg)
{
    if (U2BRG != brg) {
        uart_flush();
        uart2_setup(brg);
        return true;
    }
    return false;
}

size_t uart_write(const uint8_t *data, size_t len)
{
    size_t n = 0;
    while ((n < len) && !U2STATbits.TXBF) {
        U2TXB = data[n++];
    }
    return n;
}

void uart_flush(void)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return;     /* the simulator never sets TXMTIF: the bounded wait below
                 * would take about a minute per call and look like a hang */
#endif
    uint32_t n = UART_TX_WAIT_LIMIT;
    while (!U2STATbits.TXMTIF && (--n != 0u)) { }   /* shift reg empty too */
}

bool uart_tx_full(void)
{
    return U2STATbits.TXBF ? true : false;
}

void uart_putc(uint8_t b)
{
    U2TXB = b;
}

bool uart_rx_empty(void)
{
    return U2STATbits.RXBE ? true : false;
}

uint8_t uart_getc(void)
{
    return (uint8_t)U2RXB;
}

void uart_stat_probe(void)
{
    (void)U2STAT;
}

void uart_enable_rx_irq(uint8_t priority)
{
    /* Receive interrupt: IRQ 102, IEC3/IFS3 bit 6, priority in IPC12.
     * Enabled last, by the caller, once the application is ready for it. */
    IPC12bits.U2RXIP = priority;
    IFS3bits.U2RXIF  = 0u;
    IEC3bits.U2RXIE  = 1u;
}

/* Mask the receive interrupt for a moment and put it back: cli.c's
 * console_rx_resume() runs held-back bytes through the parser from the
 * main loop, and the parser must not be entered from the interrupt at
 * the same time. Bytes arriving meanwhile wait in the receive FIFO and
 * set U2RXIF, so the interrupt follows as soon as the enable returns. */
bool uart_rx_irq_mask(void)
{
    const bool was = (IEC3bits.U2RXIE != 0u);
    IEC3bits.U2RXIE = 0u;
    return was;
}

void uart_rx_irq_restore(bool was)
{
    if (was) { IEC3bits.U2RXIE = 1u; }
}

/* Weak default: a project that never overrides this only loses received
 * bytes, nothing else (P0.5/P4.7 weak-hook pattern - tests/trace/README.md,
 * P4.7 finding: weak with one strong override links on this toolchain). */
__attribute__((weak)) void uart_rx_hook(uint8_t byte)
{
    (void)byte;
}

/* The parser thread: every received byte goes to uart_rx_hook() (weak
 * default above; cli.c's strong override feeds the line editor). */
void __attribute__((interrupt, no_auto_psv)) _U2RXInterrupt(void)
{
    IFS3bits.U2RXIF = 0u;             /* first: a byte arriving meanwhile re-raises it */
    while (!U2STATbits.RXBE) {
        const uint8_t b = (uint8_t)U2RXB;
        uart_rx_hook(b);
    }
}

/* ------------------------------------------------------------------ *
 * Register dump - see uart.h: called from cli.c's console_regs_dump(),
 * not from diag.c directly.
 * ------------------------------------------------------------------ */
void uart_regs_visit(reg_visit_t visit)
{
    visit("[regs] uart\r\n", 0u, REG_TITLE);
    visit("IEC3", IEC3, REG_HEX);           /* U2RX enable,  bit 6       */
    visit("IFS3", IFS3, REG_HEX);           /* U2RX flag,    bit 6       */
    visit("IPC12", IPC12, REG_HEX);         /* U2RX priority, bits 26:24 */
    visit("U2CON", U2CON, REG_HEX);
    visit("U2STAT", U2STAT, REG_HEX);
    visit("U2BRG", U2BRG, REG_HEX);
    /* Pin routing of the console itself: with a silent terminal these say
     * whether uart_init()/uart_reinit() took effect. Expected: IOLOCK set,
     * the TX pin's byte in its RPOR word = 21 = 0x15 (U2TX), U2RXR (bits
     * 23:16 of RPINR13) = the RX pin's remap number (board.h: 50 = 0x32 on
     * the EV74H48A, 44 = 0x2C on the Nano), the TX pin's TRIS bit clear,
     * the RX pin's TRIS bit set. */
    visit("RPCON", RPCON, REG_HEX);
    visit("RPORn (TX pin's word)", CONSOLE_TX_RPOR_WORD, REG_HEX);
    visit("RPINRn (U2RXR's word)", CONSOLE_RX_RPINR_WORD, REG_HEX);
    visit("TRISx (TX pin's port)", CONSOLE_TX_TRIS_WORD, REG_HEX);
    visit("TRISx (RX pin's port)", CONSOLE_RX_TRIS_WORD, REG_HEX);
}

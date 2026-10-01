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
 * Since 01.10.2026 it also puts the transmit path back on polling
 * (uart_tx_polled()): its callers - the trap handler, _CLKFInterrupt
 * through clock_fail_hook() - run above the transmit interrupt, which
 * could never empty the ring under them.
 *
 * Safe to call when the console is already up - it re-writes the same
 * values - and safe from interrupt context: no waiting except the
 * bounded uart_flush(). */
void uart_reinit(uint32_t brg)
{
    uart_tx_polled();
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

/* ------------------------------------------------------------------ *
 * Transmit: a ring buffer emptied by the transmit interrupt (01.10.2026)
 *
 * Until then every byte went straight into the 8-byte transmit FIFO and
 * the writer polled TXBF until there was room - with the console's
 * commands running inside the receive interrupt, a 1.6 KB "help" held the
 * CPU for 175 ms, and the main loop missed 550 halves at 8 MSPS meanwhile
 * (docs/HARDWARE-LOG.md, 01.10.2026). Now a writer copies into tx_ring[]
 * and returns; _U2TXInterrupt() moves the bytes on into the FIFO.
 *
 * Two modes. Interrupt mode from uart_enable_tx_irq() on (cli_init(),
 * never in the simulator, which aborts on any pending interrupt); every
 * writer then runs below the transmit interrupt's priority - the main
 * loop (IPL 0) and the receive interrupt (IPL 1), where the console's
 * commands run. Polled mode before that, in the simulator, and from
 * uart_tx_polled() on - called by the three paths that print from above
 * that priority and never return (fail() through cli.c's
 * console_sync_baud(), the trap handler and _CLKFInterrupt through
 * uart_reinit()): there the transmit interrupt could never empty the
 * ring, so the ring is emptied into the FIFO by polling first (the order
 * is kept) and then the bytes go straight into the FIFO, byte for byte
 * the code of before.
 *
 * Exclusion in interrupt mode: a writer raises the interrupt-disable
 * threshold (DISICTL, DS70005591D 10.10.1.1, p596: "requests at an IPL at
 * or below the selected IPLT will be inhibited") to the transmit
 * interrupt's priority while it copies - that shuts out the interrupt and
 * every other writer (receive interrupt, main loop), and leaves the DMA
 * (4) and the counters (3) running. tx_lock() never lowers a threshold
 * someone else has raised.
 *
 * FIFO: 8 bytes; TXWM = 0 raises U2TXIF when all 8 slots are empty
 * (U2STAT bits 30:28, p1470; Example 20-1, p1495: flag cleared first,
 * the enable cleared when nothing is left to send). The shift register is
 * still sending the last byte when the FIFO runs empty, so refilling from
 * the interrupt leaves no gap on the line. TXWM = 7 ("one empty slot or
 * more") was tried for finer steps and silenced the console after two
 * characters (01.10.2026): the flag is raised on reaching the watermark,
 * not held while it is met, and with the FIFO never full again after the
 * interrupt cleared it, no new edge came. Every entry here moves at least
 * one byte into a FIFO it found empty, so the next "empty" edge always
 * follows.
 *
 * Size, 8 KB: a whole "stream grab" frame - one half, at most 2048
 * samples = 4096 bytes, plus a header of up to 173, the CRC line and the
 * prompt - fits, so the grab never waits for room. With a 2 KB ring it
 * waited inside the receive interrupt for the payload to leave, then for
 * the prompt in 8-byte steps with the stream already restarted: 2 halves
 * missed per grab at 8 MSPS (docs/HARDWARE-LOG.md, 01.10.2026). Now the
 * grab's halt lasts as long as the copy into the ring, not as long as the
 * line takes. ("blk" of the full 4096-sample buffer, 8 KB of payload,
 * waits for its last few hundred bytes - a back-to-back command the GUI
 * no longer sends.)
 *
 * And no bigger than 8 KB - more precisely, smaller than capture.c's
 * .dma_buffer section (0x2040 bytes at 2048 samples per half). The
 * linker places sections largest first; at 8.5 KB the ring went between
 * siggen.c's table and the ADC buffer, both .dma_buffer, which must lie
 * directly one on the other (dma.c: DMALOW/DMAHIGH cover both, "the
 * table lies directly below the buffer"). The buffer then sat 8.5 KB
 * higher, across 0xC000, and "chain 6" lost ~7 000 samples per second to
 * DMA overruns at 16 MSPS and ~9 700 at 20 MSPS, four runs out of four,
 * against 0 with the old layout (docs/HARDWARE-LOG.md, 01.10.2026). The
 * check after a build: xc-dsc-objdump -h shows ONE .dma_buffer section,
 * 0x6040 bytes; "siggen" reports window_gap 0 while it plays.
 * ------------------------------------------------------------------ */
#define TX_RING_SIZE  8192u                 /* see "Size" above          */

static volatile uint8_t  tx_ring[TX_RING_SIZE];
static volatile uint32_t tx_head = 0;       /* next free slot (writers)  */
static volatile uint32_t tx_tail = 0;       /* next byte to send         */
static volatile bool     tx_irq_on = false; /* interrupt mode            */
static volatile uint32_t tx_prio = 7u;      /* uart_enable_tx_irq()      */
volatile uint32_t uart_tx_dropped = 0;      /* ring full, byte lost      */

static inline uint32_t tx_next(uint32_t i) { return (i + 1u == TX_RING_SIZE) ? 0u : i + 1u; }
static inline uint32_t tx_used_of(uint32_t head, uint32_t tail)
{
    return (head >= tail) ? (head - tail) : (head + TX_RING_SIZE - tail);
}

/* Raise the interrupt-disable threshold to the transmit interrupt's
 * priority (never lower it); returns the old one for tx_unlock(). */
static inline uint32_t tx_lock(void)
{
    const uint32_t old = DISIIPL;
    (void)__builtin_write_DISICTL((old > tx_prio) ? old : tx_prio);
    return old;
}

static inline void tx_unlock(uint32_t old)
{
    (void)__builtin_write_DISICTL(old);
}

/* Ring -> FIFO, as much as fits: the interrupt's body, and the polled
 * mode's (where the interrupt is off). */
static void tx_move(void)
{
    uint32_t tail = tx_tail;
    while ((tail != tx_head) && !U2STATbits.TXBF) {
        U2TXB = tx_ring[tail];
        tail = tx_next(tail);
    }
    tx_tail = tail;
}

/* Polled mode: empty the ring into the FIFO, waiting for room as long as
 * the FIFO keeps taking bytes - bounded per byte that does not move, not
 * in total: a full ring is 180 ms of line time at 115200 baud. */
static void tx_drain_polled(void)
{
    uint32_t n = UART_TX_WAIT_LIMIT;
    while ((tx_tail != tx_head) && (n != 0u)) {
        const uint32_t before = tx_tail;
        tx_move();
        n = (tx_tail != before) ? UART_TX_WAIT_LIMIT : (n - 1u);
    }
}

void uart_tx_polled(void)
{
    IEC3bits.U2TXIE = 0u;
    tx_irq_on = false;
}

/* For the length of a measurement: the ring sent out first (so the
 * order is kept and nothing is left for the interrupt to do), then
 * polled mode; resume puts interrupt mode back if it was on. */
bool uart_tx_irq_suspend(void)
{
    const bool was = tx_irq_on;
    if (was) {
        uart_flush();
        uart_tx_polled();
    }
    return was;
}

void uart_tx_irq_resume(bool was)
{
    if (was) {
        tx_irq_on = true;
        if (tx_tail != tx_head) { IEC3bits.U2TXIE = 1u; }
    }
}

size_t uart_write(const uint8_t *data, size_t len)
{
    size_t n = 0;
    if (tx_irq_on) {
        const uint32_t old = tx_lock();
        uint32_t head = tx_head;
        const uint32_t room = (TX_RING_SIZE - 1u) - tx_used_of(head, tx_tail);
        while ((n < len) && (n < room)) {
            tx_ring[head] = data[n++];
            head = tx_next(head);
        }
        tx_head = head;
        if (n != 0u) { IEC3bits.U2TXIE = 1u; }   /* the interrupt takes it on */
        tx_unlock(old);
        return n;
    }
    tx_move();                                   /* polled: older bytes first */
    if (tx_tail != tx_head) { return 0u; }       /* the caller retries        */
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
    /* The ring first - emptied by the interrupt, or here by polling in
     * polled mode - bounded per byte that does not move. */
    uint32_t n = UART_TX_WAIT_LIMIT;
    while ((tx_tail != tx_head) && (n != 0u)) {
        const uint32_t before = tx_tail;
        if (!tx_irq_on) { tx_move(); }
        n = (tx_tail != before) ? UART_TX_WAIT_LIMIT : (n - 1u);
    }
    n = UART_TX_WAIT_LIMIT;
    while (!U2STATbits.TXMTIF && (--n != 0u)) { }   /* shift reg empty too */
}

/* "No room for one more byte": in the ring in interrupt mode; in polled
 * mode in the FIFO, after moving the ring on, so that a writer waiting on
 * this makes progress where no interrupt does it. */
bool uart_tx_full(void)
{
    if (tx_irq_on) {
        return tx_used_of(tx_head, tx_tail) == (TX_RING_SIZE - 1u);
    }
    tx_move();
    return (tx_tail != tx_head) || U2STATbits.TXBF;
}

/* One byte, after the caller has waited on uart_tx_full() (bounded): a
 * byte that still finds no room is dropped and counted, never waited for
 * - in polled mode after one more bounded drain of the ring. */
void uart_putc(uint8_t b)
{
    if (uart_write(&b, 1u) == 0u) {
        if (!tx_irq_on) {
            tx_drain_polled();
            if ((tx_tail == tx_head) && !U2STATbits.TXBF) { U2TXB = b; return; }
        }
        uart_tx_dropped++;
    }
}

void uart_enable_tx_irq(uint8_t priority)
{
    /* Transmit interrupt: IRQ 103, IEC3/IFS3 bit 7, priority in IPC12
     * (ATDF; _U2TXIE/_U2TXIF/_U2TXIP in the pack header). The flag is
     * left as it is: with the FIFO empty it is already set, and the
     * enable goes on only when the ring has something to send. */
    IEC3bits.U2TXIE  = 0u;
    IPC12bits.U2TXIP = priority;
    U2STATbits.TXWM  = 0u;            /* flag when all 8 FIFO slots are empty */
    tx_prio = priority;
    tx_irq_on = true;
    if (tx_tail != tx_head) { IEC3bits.U2TXIE = 1u; }
}

/* Ring -> FIFO. Flag first (project rule; a FIFO that empties again
 * later raises it again), the enable off once the ring is empty. No lock
 * around that check and clear: every writer that sets the enable runs
 * below this interrupt's priority and cannot get in between. */
void __attribute__((interrupt, no_auto_psv)) _U2TXInterrupt(void)
{
    IFS3bits.U2TXIF = 0u;
    tx_move();
    if (tx_tail == tx_head) { IEC3bits.U2TXIE = 0u; }
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

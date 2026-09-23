/*
 * cli.c
 *
 * Console for the ADC/DMA example: the UART, a trace channel for the
 * start-up sequence, and the command parser from
 * https://github.com/zabooh/cmd_parser (Apache 2.0). This file owns the
 * transport and the commands; the parser itself knows no hardware.
 *
 * Transport on the EV74H48A
 *   UART2 on the MCP2221A USB-UART channel: U2TX -> RH1 (RP114, DIM pin
 *   P98 "UART_USB_TX"), U2RX <- RD1 (RP50, DIM pin P96
 *   "UART_USB_RX"), 115200 8N1 (pins in board.h). The MCP2221A implements standard USB CDC
 *   and shows up on the PC as its own COM port (user guide DS70005562D
 *   2.1.1). The PPS code is the one Microchip's own example uses on
 *   this board (U2TX = 21, Table "Output Selection for Remappable
 *   Pins", p613). The board's other USB-UART channel (the PKOB4's) is
 *   not used by this console.
 *
 * Two phases
 *   console_early_init() runs before the clocks are touched, on the
 *   8 MHz FRC, so that every step of clock_init() and a failure inside
 *   it can be reported. console_puts() is the blocking trace output used
 *   from then on. cli_init() runs after the clocks: it moves the baud
 *   generator to the 100 MHz peripheral clock, starts the parser, prints
 *   the banner and enables the receive interrupt.
 *
 * The parser as its own thread
 *   Received bytes are handled in the UART2 receive interrupt, which
 *   runs at priority 1. A command executes inside that interrupt,
 *   including its output; the DMA interrupt (priority 4) preempts it, so
 *   the measurement keeps running while a long reply drains. main() is
 *   the one that waits during a long reply - it only processes buffer
 *   halves and blinks, and it reports that with proc_missed if it
 *   matters. Ctrl+C aborts a long reply: the yield hook, which runs
 *   while the transmit buffer is full, peeks at the receiver for it.
 *
 * Commands
 *   help                       list of commands (built into the parser)
 *   version                    build, board, ADC core and input
 *   status                     run state, counters, input, self-test
 *   regs                       dump of the clock, ADC, DMA and UART registers
 *   start | stop               burst stream on/off
 *   samc <0..31>               sample time, (2*SAMC + 0.5) TAD
 *   input <0..15>              PINSEL of the ADC core (6 = internal ref)
 *   selftest                   sample the internal reference, judge it
 *   stats                      min/max/mean of the completed half
 *   dump [count] [offset]      samples of the completed half
 *   clear                      zero the error counters
 *   led on|off|auto            LED0
 *   reset                      software reset
 *
 * Every command ends its output with a newline and calls
 * cmd_parser_fail() on a usage or argument error, so a script sees NAK
 * (see cmd_parser.h, "Prompt as a protocol element").
 */

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "clock.h"
#include "capture.h"
#include "led.h"
#include "diag.h"
#include "console.h"
#include "sim.h"
#include "cmd_parser.h"

/* ------------------------------------------------------------------ *
 * UART2 transport
 * ------------------------------------------------------------------ */

/* Baud rate generator, fractional mode (CLKMOD = 1): BRG = F_clk / baud,
 * no -1 (the value MCC generates for this board is 868 at 100 MHz).
 * F_clk is the Standard Speed Peripheral Clock = CPU clock / 2:
 *   after reset, on the 8 MHz FRC:        4 MHz  -> BRG 35, 114 286 baud
 *   after clock_init(), PLL2 at 200 MHz: 100 MHz -> BRG 868, 115 207 baud
 * Both are within 1 % of 115 200. */
#define UART_BRG_FRC      35u
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define UART_BRG_PLL      UART_BRG_FRC   /* the simulator never leaves the 8 MHz FRC */
#else
#define UART_BRG_PLL      868u
#endif

#define UART_RX_PRIORITY  1u      /* below the DMA interrupt (4)        */

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

void console_early_init(void)
{
    /* Pins: TX output, RX input (board.h). Peripheral pin select needs
     * IOLOCK cleared. */
    CONSOLE_TX_TRIS  = 0u;
    CONSOLE_RX_TRIS  = 1u;
    RPCONbits.IOLOCK = 0u;
    CONSOLE_RX_RPINR = CONSOLE_RX_RP;
    CONSOLE_TX_RPOR  = CONSOLE_TX_FN;
    RPCONbits.IOLOCK = 1u;

    uart2_setup(UART_BRG_FRC);
    console_puts("\r\n[boot] uart up on FRC, 115200 8N1\r\n");
}

/* How long to wait for the transmitter, in polling iterations. One
 * character takes 87 us at 115200 baud; on the 8 MHz FRC that is a few
 * hundred cycles, at 200 MHz about 17 000. 200 000 is far more than
 * either and still finite - which is the whole point: console_puts() is
 * called from _DefaultInterrupt() to report a trap, and if the UART is
 * not actually transmitting (wrong baud divider, ON bit cleared, clock
 * gone) an unbounded wait would silently swallow the one message that
 * explains the fault. Better a garbled line than none. */
#define TX_WAIT_LIMIT     200000u

/* Blocking output, usable at any time after console_early_init(): from
 * main(), from fail(), from _DefaultInterrupt() and from the receive
 * interrupt (the parser's own output goes through console_write()
 * below instead). Never blocks forever - see TX_WAIT_LIMIT. */
void console_puts(const char *s)
{
    while (*s) {
        uint32_t n = TX_WAIT_LIMIT;
        while (U2STATbits.TXBF && (--n != 0u)) { }
        U2TXB = (uint8_t)*s++;
    }
}

static void console_drain(void)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return;     /* the simulator never sets TXMTIF: the bounded wait below
                 * would take about a minute per call and look like a hang */
#endif
    uint32_t n = TX_WAIT_LIMIT;
    while (!U2STATbits.TXMTIF && (--n != 0u)) { }   /* shift reg empty too */
}

/* Bring the console back up from scratch, assuming nothing about the
 * current state of the pins, the PPS mapping or the UART.
 *
 * This is what _DefaultInterrupt() calls before it reports a trap. A trap
 * can have happened anywhere, including inside clock_init() or after some
 * other code disturbed the peripheral, and in that situation
 * console_sync_baud() is not enough: it only fixes the baud divider, and
 * it trusts clock.c to say what the clock is. Here the routing is written
 * again and the baud rate is picked from the clock the CPU is actually
 * on, so the one message that explains the fault has the best chance of
 * getting out.
 *
 * Safe to call when the console is already up - it re-writes the same
 * values - and safe from interrupt context: no waiting except the bounded
 * drain. */
void console_force_up(void)
{
    console_drain();                   /* bounded; keep a partial line    */

    CONSOLE_TX_TRIS  = 0u;
    CONSOLE_RX_TRIS  = 1u;
    RPCONbits.IOLOCK = 0u;
    CONSOLE_RX_RPINR = CONSOLE_RX_RP;
    CONSOLE_TX_RPOR  = CONSOLE_TX_FN;
    RPCONbits.IOLOCK = 1u;

    uart2_setup(clock_cpu_on_pll() ? UART_BRG_PLL : UART_BRG_FRC);
}

/* Make the baud generator match whatever clock the CPU is on right now.
 * fail() calls this first: a failure after the switch to PLL2 but before
 * cli_init() would otherwise print at the wrong rate. */
void console_sync_baud(void)
{
    const uint32_t want = clock_cpu_on_pll() ? UART_BRG_PLL : UART_BRG_FRC;
    if (U2BRG != want) {
        console_drain();
        uart2_setup(want);
    }
}

/* Output sink for the parser: take what fits into the transmit FIFO and
 * report how much that was. The parser re-offers the rest (see
 * cmd_parser.h, "Flow control"). */
static size_t console_write(const char *data, size_t len)
{
    size_t n = 0;
    while ((n < len) && !U2STATbits.TXBF) {
        U2TXB = (uint8_t)data[n++];
    }
    return n;
}

/* Called by the parser while the transmit FIFO is full. Nothing to yield
 * to on a bare-metal build, but this is the moment to look for Ctrl+C:
 * a long reply must be abortable, and while it drains the receive
 * interrupt cannot run (we are inside it). Other bytes typed during a
 * reply are dropped. */
static void console_yield(void)
{
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    return;     /* the simulator has no UART receiver: RXBE never rises */
#endif
    while (!U2STATbits.RXBE) {
        if ((uint8_t)U2RXB == 0x03u) {
            cmd_parser_abort();
        }
    }
}

/* The parser thread: every received byte goes to the line editor, and
 * a completed line is dispatched right here, in interrupt context. */
void __attribute__((interrupt, no_auto_psv)) _U2RXInterrupt(void)
{
    while (!U2STATbits.RXBE) {
        cmd_parser_feed_char((char)U2RXB);
    }
    IFS3bits.U2RXIF = 0u;
}

/* ------------------------------------------------------------------ *
 * Small formatting helpers - no printf, so the reply cost is predictable
 * ------------------------------------------------------------------ */
static char *u32_to_str(char *out, uint32_t v)
{
    char tmp[11];
    int i = 0;
    do { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u);
    while (i > 0) { *out++ = tmp[--i]; }
    *out = '\0';
    return out;
}

static char *u32_to_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    *out++ = '0'; *out++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        *out++ = digits[(v >> shift) & 0xFu];
    }
    *out = '\0';
    return out;
}

static char *copy_str(char *out, const char *s)
{
    while (*s) { *out++ = *s++; }
    *out = '\0';
    return out;
}

/* "key: value" lines.
 *
 * The key is NOT copied into the line buffer. It used to be, into a
 * char[48], and two callers overflowed it: the self-test's
 * "[selftest] mean on internal 15/16 VDD (expect ~3840)" is 52
 * characters, so with the value and CRLF it wrote 67 bytes into 48 - a
 * stack overrun on the *success* path, which would have corrupted the
 * return address at the exact moment the board reported that everything
 * worked. Printing the key straight out and formatting only the number
 * removes the failure mode instead of enlarging the buffer: the value is
 * at most 10 digits or 10 hex characters, so 16 bytes is provably enough
 * no matter how long a key some later caller passes.
 *
 * The console_* variants go out blocking through console_puts() (trace,
 * fail, trap, dump); put_kv() goes through the parser and is for command
 * replies. */
void console_kv(const char *key, uint32_t v)
{
    char num[16];
    char *p = u32_to_str(num, v);
    copy_str(p, "\r\n");
    console_puts(key);
    console_puts(": ");
    console_puts(num);
}

void console_kv_hex(const char *key, uint32_t v)
{
    char num[16];
    char *p = u32_to_hex(num, v);
    copy_str(p, "\r\n");
    console_puts(key);
    console_puts(": ");
    console_puts(num);
}

void console_regs_dump(void)
{
    console_puts("[regs] uart\r\n");
    console_kv_hex("IEC3", IEC3);           /* U2RX enable,  bit 6       */
    console_kv_hex("IFS3", IFS3);           /* U2RX flag,    bit 6       */
    console_kv_hex("IPC12", IPC12);         /* U2RX priority, bits 26:24 */
    console_kv_hex("U2CON", U2CON);
    console_kv_hex("U2STAT", U2STAT);
    console_kv_hex("U2BRG", U2BRG);
    /* Pin routing of the console itself: with a silent terminal these say
     * whether console_early_init() took effect. Expected: IOLOCK set,
     * RP114R (bits 14:8 of RPOR28) = 21 = 0x15, U2RXR (bits 23:16 of
     * RPINR13) = 50 = 0x32, TRISH bit 1 clear, TRISD bit 1 set. */
    console_kv_hex("RPCON", RPCON);
    console_kv_hex("RPOR28", RPOR28);
    console_kv_hex("RPINR13", RPINR13);
    console_kv_hex("TRISH", TRISH);
    console_kv_hex("TRISD", TRISD);
}

static void put_kv(const char *key, uint32_t v)
{
    char num[16];
    char *p = u32_to_str(num, v);
    copy_str(p, "\r\n");
    cmd_parser_write(key);
    cmd_parser_write(": ");
    cmd_parser_write(num);
}

static void put_line(const char *s)
{
    cmd_parser_write(s);
    cmd_parser_write("\r\n");
}

/* One status line, blocking, for the periodic trace from main(). */
void console_status_line(void)
{
    char line[176];                       /* 163 used with every field at max */
    char *p = copy_str(line, "[stat] blocks=");
    p = u32_to_str(p, blocks_done);
    p = copy_str(p, " overrun=");  p = u32_to_str(p, dma_overrun);
    p = copy_str(p, " late=");     p = u32_to_str(p, late_service);
    p = copy_str(p, " missed=");   p = u32_to_str(p, proc_missed);
    p = copy_str(p, " addr_err="); p = u32_to_str(p, dma_addr_err);
    p = copy_str(p, " bus_err=");  p = u32_to_str(p, dma_bus_err);
    p = copy_str(p, " last=");     p = u32_to_str(p, last_sample);
    p = copy_str(p, " input=");    p = u32_to_str(p, capture_pinsel());
    p = copy_str(p, " samc=");     p = u32_to_str(p, capture_samc());
    p = copy_str(p, " run=");      p = u32_to_str(p, capture_running() ? 1u : 0u);
    /* Does the ADC's channel-done event reach the CPU side at all? It is
     * the DMA trigger and stays masked (IEC6 = 0), so this flag being 1
     * while the DMA runs says the event is visible to the CPU - the
     * precondition for the vector-201 trap TROUBLESHOOTING 2.0b describes. */
    p = copy_str(p, " ad3if=");    p = u32_to_str(p, (uint32_t)IFS6bits.AD3CH0IF);
    copy_str(p, "\r\n");
    console_puts(line);
}

/* Decimal argument in [lo, hi]; false (and NAK) otherwise. */
static bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
    char *end;
    unsigned long v;
    if (*s == '\0') { return false; }
    v = strtoul(s, &end, 0);
    if ((*end != '\0') || (v < lo) || (v > hi)) { return false; }
    *out = (uint32_t)v;
    return true;
}

static void usage(const char *text)
{
    cmd_parser_write("usage: ");
    put_line(text);
    cmd_parser_fail();
}

/* ------------------------------------------------------------------ *
 * Commands
 * ------------------------------------------------------------------ */
static void cmd_version_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    put_line("adc_dma_40msps " __DATE__ " " __TIME__);
    put_line("board: EV74H48A, dsPIC33AK512MPS512 GP DIM");
    put_kv("adc core", ADC_INSTANCE);
    put_kv("default input", ADC_PINSEL);
    put_kv("samples per half", SAMPLES_PER_HALF);
}
CMD_DEFINE(version, "version", cmd_version_fn, "version - build, board, ADC core");

static void cmd_status_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    put_kv("running", capture_running() ? 1u : 0u);
    put_kv("blocks", blocks_done);
    put_kv("overrun", dma_overrun);
    put_kv("late", late_service);
    put_kv("missed", proc_missed);
    put_kv("addr_err", dma_addr_err);
    put_kv("bus_err", dma_bus_err);
    put_kv("input", capture_pinsel());
    put_kv("samc", capture_samc());
    put_kv("last", last_sample);
    put_kv("selftest_mean", selftest_mean);
    put_kv("fail_code", fail_code);
}
CMD_DEFINE(status, "status", cmd_status_fn, "status - run state and counters");

static void cmd_regs_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    regs_dump();
}
CMD_DEFINE(regs, "regs", cmd_regs_fn, "regs - clock, ADC, DMA and UART registers");

static void cmd_start_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    capture_start();
    put_kv("running", 1u);
}
CMD_DEFINE(start, "start", cmd_start_fn, "start - start the burst stream");

static void cmd_stop_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    capture_stop();
    put_kv("running", 0u);
}
CMD_DEFINE(stop, "stop", cmd_stop_fn, "stop - stop after the current buffer");

static void cmd_samc_fn(int argc, char **argv)
{
    uint32_t v;
    if ((argc != 2) || !arg_u32(argv[1], 0u, 31u, &v)) {
        usage("samc <0..31>  (sample time (2*SAMC+0.5) TAD, rate 40/(SAMC+1) MSPS)");
        return;
    }
    (void)capture_set_input(capture_pinsel(), (uint8_t)v);
    put_kv("samc", v);
}
CMD_DEFINE(samc, "samc", cmd_samc_fn, "samc <0..31> - sample time in TAD steps");

static void cmd_input_fn(int argc, char **argv)
{
    uint32_t v;
    if ((argc != 2) || !arg_u32(argv[1], 0u, 15u, &v)) {
        usage("input <0..15>  (PINSEL of the ADC core, 6 = internal 15/16 VDD)");
        return;
    }
    (void)capture_set_input((uint8_t)v, capture_samc());
    put_kv("input", v);
}
CMD_DEFINE(input, "input", cmd_input_fn, "input <0..15> - analog input (PINSEL)");

static void cmd_selftest_fn(int argc, char **argv)
{
    uint32_t mean = 0;
    (void)argc; (void)argv;
    const uint32_t rc = capture_selftest(&mean);
    put_kv("selftest_mean", mean);
    if (rc == 0u)      { put_line("selftest: ok (3648..4032)"); }
    else if (rc == 6u) { put_line("selftest: no data - is the stream running?"); }
    else if (rc == 7u) { put_line("selftest: mean outside 3648..4032"); }
    else               { put_line("selftest: DMA channel disabled (address fault?)"); }
    if (rc != 0u) { cmd_parser_fail(); }
}
CMD_DEFINE(selftest, "selftest", cmd_selftest_fn, "selftest - sample the internal reference");

static void cmd_stats_fn(int argc, char **argv)
{
    const volatile uint16_t *b = capture_completed_half();
    uint32_t mn = 0xFFFFu, mx = 0u, acc = 0u;
    (void)argc; (void)argv;
    for (uint32_t i = 0; i < SAMPLES_PER_HALF; i++) {
        const uint16_t v = b[i];
        if (v < mn) { mn = v; }
        if (v > mx) { mx = v; }
        acc += v;
    }
    put_kv("half", ready_half);
    put_kv("min", mn);
    put_kv("max", mx);
    put_kv("mean", acc / SAMPLES_PER_HALF);
    put_kv("pp", mx - mn);
}
CMD_DEFINE(stats, "stats", cmd_stats_fn, "stats - min/max/mean of the completed half");

static void cmd_dump_fn(int argc, char **argv)
{
    uint32_t count = 64u, offset = 0u;
    if ((argc > 3) ||
        ((argc >= 2) && !arg_u32(argv[1], 1u, SAMPLES_PER_HALF, &count)) ||
        ((argc == 3) && !arg_u32(argv[2], 0u, SAMPLES_PER_HALF - 1u, &offset))) {
        usage("dump [count 1..1024] [offset 0..1023]");
        return;
    }
    if (offset + count > SAMPLES_PER_HALF) {
        count = SAMPLES_PER_HALF - offset;
    }
    const volatile uint16_t *b = capture_completed_half();
    char line[80];
    for (uint32_t i = 0; i < count; i += 8u) {
        char *p = line;
        /* "nnnn:" index, then up to eight values */
        uint32_t idx = offset + i;
        for (int d = 3; d >= 0; d--) { p[d] = (char)('0' + idx % 10u); idx /= 10u; }
        p += 4; *p++ = ':';
        for (uint32_t k = 0; (k < 8u) && (i + k < count); k++) {
            *p++ = ' ';
            p = u32_to_str(p, b[offset + i + k]);
        }
        copy_str(p, "\r\n");
        cmd_parser_write(line);
        if (cmd_parser_aborted()) { return; }
    }
}
CMD_DEFINE(dump, "dump", cmd_dump_fn, "dump [count] [offset] - samples of the completed half");

static void cmd_clear_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    counters_clear();
    put_line("counters cleared");
}
CMD_DEFINE(clear, "clear", cmd_clear_fn, "clear - zero the error counters");

static void cmd_led_fn(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0)   { led_mode(1u); put_line("led: on");   return; }
        if (strcmp(argv[1], "off") == 0)  { led_mode(0u); put_line("led: off");  return; }
        if (strcmp(argv[1], "auto") == 0) { led_mode(2u); put_line("led: auto"); return; }
    }
    usage("led on|off|auto");
}
CMD_DEFINE(led, "led", cmd_led_fn, "led on|off|auto - LED0");

static void cmd_reset_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    put_line("resetting");
    console_drain();                    /* let the reply leave first */
    __asm__ volatile ("reset");
}
CMD_DEFINE(reset, "reset", cmd_reset_fn, "reset - software reset");

/* ------------------------------------------------------------------ */
void cli_init(void)
{
    /* The clocks have changed under the baud generator: re-set it for
     * the 100 MHz peripheral clock, after the last FRC-timed byte is out.
     * Only when the divider really changes: in the simulator build the
     * CPU never leaves the FRC, and toggling ON while the simulator's
     * UART model is still transmitting leaves its transmitter dead
     * (TXWRE set, nothing gets out any more). */
    if (U2BRG != UART_BRG_PLL) {
        console_drain();
        uart2_setup(UART_BRG_PLL);
        console_puts("[boot] uart reclocked to PLL2, 115200 8N1\r\n");
    }

    cmd_parser_init(console_write);
    cmd_parser_set_yield(console_yield);     /* after init - init clears it */
    (void)cmd_register(&cmd_version);
    (void)cmd_register(&cmd_status);
    (void)cmd_register(&cmd_regs);
    (void)cmd_register(&cmd_start);
    (void)cmd_register(&cmd_stop);
    (void)cmd_register(&cmd_samc);
    (void)cmd_register(&cmd_input);
    (void)cmd_register(&cmd_selftest);
    (void)cmd_register(&cmd_stats);
    (void)cmd_register(&cmd_dump);
    (void)cmd_register(&cmd_clear);
    (void)cmd_register(&cmd_led);
    (void)cmd_register(&cmd_reset);

    /* Banner, once at start-up. A human sees what is talking and which
     * build it is; a script simply reads on until the readiness byte that
     * follows the first prompt. */
    console_puts("\r\n"
                 "adc_dma_40msps - ADC at 40 MSPS into RAM via DMA\r\n"
                 SIM_BANNER_NOTE           /* empty on silicon            */
                 "board: EV74H48A, dsPIC33AK512MPS512 GP DIM\r\n"
                 "build: " __DATE__ " " __TIME__ "\r\n"
                 "type 'help' for the commands\r\n"
                 "please log this terminal from power-up and send it back\r\n");

    /* Receive interrupt: IRQ 102, IEC3/IFS3 bit 6, priority in IPC12.
     * Enabled last, so that nothing typed early runs a command before
     * the measurement is set up. */
    IPC12bits.U2RXIP = UART_RX_PRIORITY;
    IFS3bits.U2RXIF  = 0u;
    IEC3bits.U2RXIE  = 1u;

    cmd_parser_prompt();                     /* sync point for a reader */
}

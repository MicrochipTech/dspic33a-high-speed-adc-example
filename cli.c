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
 *   clk <100..1000>            ADC clock divide ratio x 100 - the sample rate
 *   test [part] [halves]       run a part of the measurement, or "all"
 *   input <0..15>              PINSEL of the ADC core (6 = internal ref)
 *   selftest                   sample the internal reference, judge it
 *   stats                      min/max/mean of the completed half
 *   dump [count] [offset]      samples of the completed half
 *   clear                      zero the error counters
 *   led on|off|auto            LED0
 *   sweep [halves]             overrun vs sample rate, 1.25..40 MSPS, one table
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
#include "adc.h"
#include "timebase.h"
#include "clock.h"
#include "capture.h"
#include "adc.h"
#include "dac.h"
#include "dactest.h"
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

void console_flush(void)
{
    console_drain();
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

/* Receive diagnostics, shown in the [stat] line and by "status": how many
 * bytes the interrupt took from the UART, the last one, and how many
 * CR / LF among them. "I type and nothing happens" is then one of three
 * things: rx stays 0 (nothing reaches RD1, the echo was the terminal's),
 * rx counts but cr stays 0 (the terminal sends LF only - the parser ends
 * a line on CR), or cr counts and still no reply (the parser or the
 * transmit path). */
static volatile uint32_t rx_count = 0;
static volatile uint32_t rx_cr    = 0;
static volatile uint32_t rx_lf    = 0;
static volatile uint8_t  rx_last  = 0;

/* The parser thread: every received byte goes to the line editor, and
 * a completed line is dispatched right here, in interrupt context. */
void __attribute__((interrupt, no_auto_psv)) _U2RXInterrupt(void)
{
    IFS3bits.U2RXIF = 0u;             /* first: a byte arriving meanwhile re-raises it */
    while (!U2STATbits.RXBE) {
        const uint8_t b = (uint8_t)U2RXB;
        rx_count++;
        rx_last = b;
        if (b == 0x0Du)      { rx_cr++; }
        else if (b == 0x0Au) { rx_lf++; }
        cmd_parser_feed_char((char)b);
    }
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
     * whether console_early_init() took effect. Expected: IOLOCK set, the
     * TX pin's byte in its RPOR word = 21 = 0x15 (U2TX), U2RXR (bits 23:16
     * of RPINR13) = the RX pin's remap number (board.h: 50 = 0x32 on the
     * EV74H48A, 44 = 0x2C on the Nano), the TX pin's TRIS bit clear, the
     * RX pin's TRIS bit set. */
    console_kv_hex("RPCON", RPCON);
    console_kv_hex("RPORn (TX pin's word)", CONSOLE_TX_RPOR_WORD);
    console_kv_hex("RPINRn (U2RXR's word)", CONSOLE_RX_RPINR_WORD);
    console_kv_hex("TRISx (TX pin's port)", CONSOLE_TX_TRIS_WORD);
    console_kv_hex("TRISx (RX pin's port)", CONSOLE_RX_TRIS_WORD);
}

#if BOOT_VERBOSE
void console_trace(const char *s)                    { console_puts(s); }
void console_trace_kv(const char *key, uint32_t v)   { console_kv(key, v); }
void console_trace_kv_hex(const char *key, uint32_t v) { console_kv_hex(key, v); }
#else
void console_trace(const char *s)                    { (void)s; }
void console_trace_kv(const char *key, uint32_t v)   { (void)key; (void)v; }
void console_trace_kv_hex(const char *key, uint32_t v) { (void)key; (void)v; }
#endif

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
    char line[256];                       /* 246 used with every field at max */
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
    p = copy_str(p, " clk=");      p = u32_to_str(p, capture_clkdiv());
    p = copy_str(p, " ksps=");     p = u32_to_str(p, capture_nominal_ksps(capture_clkdiv()));
    p = copy_str(p, " run=");      p = u32_to_str(p, capture_running() ? 1u : 0u);
    p = copy_str(p, " pwr=");      p = u32_to_str(p, capture_powered() ? 1u : 0u);
    p = copy_str(p, " half=");     p = u32_to_str(p, capture_half_len());
    /* Does the ADC's channel-done event reach the CPU side at all? It is
     * the DMA trigger and stays masked (IEC6 = 0), so this flag being 1
     * while the DMA runs says the event is visible to the CPU - the
     * precondition for the vector-201 trap TROUBLESHOOTING 2.0b describes. */
    p = copy_str(p, " core=");     p = u32_to_str(p, adc_core());
    p = copy_str(p, " adif=");     p = u32_to_str(p, adc_ch0_flag() ? 1u : 0u);
    /* Receive diagnostics, see rx_count above. */
    p = copy_str(p, " rx=");       p = u32_to_str(p, rx_count);
    p = copy_str(p, " last=");     p = u32_to_hex(p, rx_last);
    p = copy_str(p, " cr=");       p = u32_to_str(p, rx_cr);
    p = copy_str(p, " lf=");       p = u32_to_str(p, rx_lf);
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
    diag_report_build();          /* same block as at boot ([build] ...) */
    put_line("input: " BOARD_INPUT_NAME);
    put_line("console: " CONSOLE_PORT_NAME);
}
CMD_DEFINE(version, "version", cmd_version_fn, "version - build id, git revision, board, configuration");

static void cmd_status_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    put_kv("running", capture_running() ? 1u : 0u);
    put_kv("powered", capture_powered() ? 1u : 0u);
    put_kv("blocks", blocks_done);
    put_kv("overrun", dma_overrun);
    put_kv("late", late_service);
    put_kv("missed", proc_missed);
    put_kv("addr_err", dma_addr_err);
    put_kv("bus_err", dma_bus_err);
    put_kv("input", capture_pinsel());
    put_kv("samc", capture_samc());
    put_kv("clkdiv (x100, read back)", capture_clkdiv());
    put_kv("clkdiv asked for", capture_clkdiv_wanted());
    put_kv("ksps nominal", capture_nominal_ksps(capture_clkdiv()));
    put_kv("adc clock Hz", clock_adc_hz());
    put_kv("last", last_sample);
    put_kv("selftest_mean", selftest_mean);
    put_kv("fail_code", fail_code);
    put_kv("rx_bytes", rx_count);
    put_kv("rx_last", rx_last);
    put_kv("rx_cr", rx_cr);
    put_kv("rx_lf", rx_lf);
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
        usage("samc <0..31>  (sample time (2*SAMC+0.5) TAD; the rate is set by 'period')");
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

static void cmd_core_fn(int argc, char **argv)
{
    uint32_t core, pinsel = ADC_PINSEL;
    if ((argc < 2) || (argc > 3) || !arg_u32(argv[1], 1u, 5u, &core) ||
        ((argc == 3) && !arg_u32(argv[2], 0u, 15u, &pinsel))) {
        usage("core <1..5> [pinsel]  (switch the ADC core; 5 3 = DAC2's pin RA8)");
        return;
    }
    if (!capture_select_core((uint8_t)core, (uint8_t)pinsel, capture_samc())) { cmd_parser_fail(); return; }
    put_kv("core", core);
    put_kv("input", pinsel);
}
CMD_DEFINE(core, "core", cmd_core_fn, "core <1..5> [pinsel] - switch the ADC core");

static void cmd_buf_fn(int argc, char **argv)
{
    uint32_t n;
    if (argc == 1) {
        put_kv("samples per half", capture_half_len());
        put_kv("maximum", SAMPLES_PER_HALF_MAX);
        return;
    }
    if ((argc != 2) || !arg_u32(argv[1], SAMPLES_PER_HALF_MIN, SAMPLES_PER_HALF_MAX, &n)) {
        usage("buf [samples per half 16..1024]  (stop first; the next start uses the new size)");
        return;
    }
    if (!capture_set_half_len(n)) {
        put_line("buf: stop the stream first");
        cmd_parser_fail();
        return;
    }
    put_kv("samples per half", capture_half_len());
}
CMD_DEFINE(buf, "buf", cmd_buf_fn, "buf [n] - samples per buffer half (16..1024)");

static void cmd_dac_fn(int argc, char **argv)
{
    uint32_t unit, low = 0x100u, high = 0xF00u, slp = 8u;
    if ((argc < 3) || !arg_u32(argv[1], 1u, DAC_UNITS, &unit)) {
        usage("dac <1|2> <on|off> [low] [high] [slpdat]  (triangle on DACOUT1 = RA1 or DACOUT2 = RA8)");
        return;
    }
    if ((argv[2][0] == 'o') && (argv[2][1] == 'f')) {
        dac_off((uint8_t)unit);
        put_kv("dac", unit);
        put_line("off");
        return;
    }
    if ((argv[2][0] != 'o') || (argv[2][1] != 'n') || (argc > 6) ||
        ((argc >= 4) && !arg_u32(argv[3], 0u, 4095u, &low)) ||
        ((argc >= 5) && !arg_u32(argv[4], 0u, 4095u, &high)) ||
        ((argc == 6) && !arg_u32(argv[5], 1u, 255u, &slp)) ||
        (high <= low)) {
        usage("dac <1|2> <on|off> [low] [high] [slpdat]  (0..4095, high > low, slpdat 1..255)");
        return;
    }
    if (!dac_triangle_start((uint8_t)unit, (uint16_t)low, (uint16_t)high, (uint16_t)slp)) {
        put_line("dac: CLKGEN7 did not come up");
        cmd_parser_fail();
        return;
    }
    put_kv("dac", unit);
    put_line(dac_pin_name((uint8_t)unit));
    put_kv("low", low);
    put_kv("high", high);
    put_kv("slpdat", slp);
    put_kv("period ns", dac_period_ns((uint8_t)unit));
}
CMD_DEFINE(dac, "dac", cmd_dac_fn, "dac <1|2> <on|off> [low] [high] [slpdat] - triangle on DACOUT1/2");

/* The DAC test measures the DAC inside the chip: UREFCON puts DAC2 on
 * the UREF line and the ADC samples it as AN7, which every core has
 * (board.h). So no core is switched and no pin is involved - run 10 ran
 * the test on core 3 against RA8, which belongs to core 5, and measured
 * an open pin. The input in use is restored afterwards, so a "dactest"
 * from the console does not silently leave the measurement elsewhere. */
static uint32_t run_dactest(uint32_t halves)
{
    const uint8_t pinsel_before = capture_pinsel();
    const uint8_t samc_before   = capture_samc();

    if (!uref_route_dac2(false)) {
        put_line("dactest: UREFCON did not take the DAC2 selection");
        return 1u;
    }
    console_kv("[dactest] DAC2 routed to the internal UREF line, INSEL", uref_insel());
    console_kv("[dactest]   measured on this core's AN7, ADC core", adc_core());
    if (!capture_set_input(DAC_UREF_PINSEL, samc_before)) {
        put_line("dactest: could not select the UREF input");
        uref_off();
        return 1u;
    }
    const uint32_t rc = dactest_run(halves);
    (void)capture_set_input(pinsel_before, samc_before);
    uref_off();
    return rc;
}

static void cmd_dactest_fn(int argc, char **argv)
{
    uint32_t halves = 64u;
    if ((argc > 2) || ((argc == 2) && !arg_u32(argv[1], 1u, 10000u, &halves))) {
        usage("dactest [halves]  (capture and judge the DAC2 triangle, default 64 halves)");
        return;
    }
    if (run_dactest(halves) != 0u) { cmd_parser_fail(); }
}
CMD_DEFINE(dactest, "dactest", cmd_dactest_fn, "dactest [halves] - judge the running DAC's triangle through the chain");

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

/* min/max/mean/pp of the completed half - shared by "stats" and the
 * periodic [half] line. */
static void half_stats(uint32_t *mn, uint32_t *mx, uint32_t *mean)
{
    const volatile uint16_t *b = capture_completed_half();
    uint32_t lo = 0xFFFFu, hi = 0u, acc = 0u;
    const uint32_t n = capture_half_len();
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t v = b[i];
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
        acc += v;
    }
    *mn = lo; *mx = hi; *mean = acc / n;
}

void console_half_stats(void)
{
    char line[96];
    uint32_t mn, mx, mean;
    half_stats(&mn, &mx, &mean);
    char *p = copy_str(line, "[half] n=");
    p = u32_to_str(p, ready_half);
    p = copy_str(p, " min=");  p = u32_to_str(p, mn);
    p = copy_str(p, " max=");  p = u32_to_str(p, mx);
    p = copy_str(p, " mean="); p = u32_to_str(p, mean);
    p = copy_str(p, " pp=");   p = u32_to_str(p, mx - mn);
    copy_str(p, "\r\n");
    console_puts(line);
}

static void cmd_stats_fn(int argc, char **argv)
{
    uint32_t mn, mx, mean;
    (void)argc; (void)argv;
    half_stats(&mn, &mx, &mean);
    put_kv("half", ready_half);
    put_kv("min", mn);
    put_kv("max", mx);
    put_kv("mean", mean);
    put_kv("pp", mx - mn);
}
CMD_DEFINE(stats, "stats", cmd_stats_fn, "stats - min/max/mean of the completed half");

static void cmd_dump_fn(int argc, char **argv)
{
    /* The WHOLE buffer, not one half. After "snap" the buffer holds one
     * contiguous window that nothing is writing any more, and that is the
     * only thing worth plotting or transforming - a half read out while
     * the stream runs is torn, because at these rates the main loop is
     * milliseconds behind the DMA (docs/HARDWARE-LOG.md run 11). */
    const uint32_t total = 2u * capture_half_len();
    uint32_t count = 64u, offset = 0u;
    if ((argc > 3) ||
        ((argc >= 2) && !arg_u32(argv[1], 1u, total, &count)) ||
        ((argc == 3) && !arg_u32(argv[2], 0u, total - 1u, &offset))) {
        usage("dump [count 1..2048] [offset 0..2047]  (the whole buffer; use snap first)");
        return;
    }
    if ((offset + count) > total) {
        count = total - offset;
    }
    const volatile uint16_t *b = capture_buffer();
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
CMD_DEFINE(dump, "dump", cmd_dump_fn, "dump [count] [offset] - samples of the buffer (snap first)");

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

/* ------------------------------------------------------------------ *
 * sweep - the rate measurement, automated
 *
 * For each divide ratio of the ladder (capture.c), slowest rate first,
 * three runs of `halves` halves: the CPU idle, the CPU doing the
 * main-loop processing, and the CPU polling an SFR. The counters after
 * each run say whether the DMA kept up (overrun) and whether the CPU
 * got every half (missed). The clock is switched once per row and read
 * back; a row whose switch did not arrive prints that and no numbers.
 * Three times per rate, with the CPU doing something different while
 * the DMA runs, because the first board run could not tell whether the
 * overruns come from the DMA bus itself or from the CPU competing for
 * it (DS70005591D 13.4.4, one shared DMA data bus):
 *
 *   idle     the CPU polls blocks_done, a RAM variable, nothing else
 *   process  the CPU runs capture_service(), i.e. process_buffer() on
 *            every completed half - what the application would do
 *   sfr      the CPU reads U2STAT in a tight loop - the worst case, a
 *            CPU that hammers the peripheral bus (the console does this
 *            while it prints)
 *
 * One line per rate. overrun must be 0 for a rate to be usable. The
 * whole sweep runs inside the receive interrupt, like every command;
 * the DMA interrupt preempts it, the main loop is starved meanwhile
 * (counters are cleared afterwards, so that does not show up). The
 * previous sample time and run state are restored at the end.
 * ------------------------------------------------------------------ */
#define SWEEP_HALVES_DEFAULT  2000u          /* 2 M samples per point   */
#define SWEEP_WAIT_LIMIT      400000000u     /* loop iterations, ~10 s  */

/* The MEASURED rate comes from timebase.c (Timer1 at 12.5 MHz). The rate
 * the first board sweep printed was the nominal 40/(SAMC+1); the counters
 * said it was not what the ADC did (equal overruns at every "rate",
 * halves missed at 1.25 MSPS), so from then on the sweep measures. */

enum sweep_load { SWEEP_IDLE = 0, SWEEP_PROCESS = 1, SWEEP_SFR = 2 };

/* Run `halves` halves at `samc` with the CPU under `load` meanwhile.
 * *ticks receives the Timer1 ticks the halves took. Returns false if
 * the stream stopped or never delivered. */
static bool sweep_point(uint32_t halves, enum sweep_load load, uint32_t *ticks)
{
    uint32_t n = SWEEP_WAIT_LIMIT;
    (void)capture_settle();                                   /* defined start     */
    counters_clear();
    const uint32_t target = blocks_done + halves;
    const uint32_t t0 = timebase_ticks();
    capture_start();
    n = SWEEP_WAIT_LIMIT;
    while (blocks_done < target) {
        SIM_DMA_TICK();
        if (load == SWEEP_PROCESS)      { (void)capture_service(); }
        else if (load == SWEEP_SFR)     { (void)U2STAT; }
        /* The brake fired: this rate floods the CPU with overrun
         * interrupts and is unusable. Not an error of the point. */
        if (capture_overrun_aborted()) { (void)capture_settle(); return false; }
        if (--n == 0u) { (void)capture_settle(); return false; }
    }
    *ticks = timebase_ticks() - t0;   /* unsigned: wrap-safe             */
    (void)capture_settle();           /* point over: DMA down            */
    return true;
}

/* Returns true if the process run delivered every half with no overrun. */
static bool sweep_row(struct pll_step st, uint32_t halves)
{
    /* The clock is switched once for the row, not once per load: the
     * three loads differ in what the CPU does, not in the rate. If the
     * switch does not arrive, the row says so and no number is printed -
     * a measured rate under an unknown divider is what made run 7
     * unreadable. */
    const uint32_t rc = capture_set_pll(st.p1, st.p2);
    if (rc != CLKDIV_OK) {
        /* Two single digits plus the longest error text of about 50:
         * well inside 144. */
        char bad[144];
        char *q = copy_str(bad, "postdiv ");  q = u32_to_str(q, st.p1);
        *q++ = '/';                           q = u32_to_str(q, st.p2);
        q = copy_str(q, ": clock switch FAILED - ");
        q = copy_str(q, clock_adc_div_error(rc));
        copy_str(q, "\r\n");
        console_puts(bad);
        return false;
    }

    /* The rate, measured on ONE burst with nothing else running. The
     * three loaded runs below cannot measure it: at a rate that overruns,
     * the CPU drowns in the overrun interrupt and the loaded figure came
     * out ten times too high at every setting (runs 8 to 11 reported
     * 42 MSPS everywhere, while a clean burst at the same setting
     * measured 3990 ksps against 4081 nominal - run 13). Both are printed,
     * because the difference is the artefact. */
    uint32_t clean_ksps = 0u;
    {
        const uint32_t nn = 2u * capture_half_len();
        const uint32_t t0 = timebase_ticks();
        if (capture_oneshot() == 0u) {
            const uint32_t dt = timebase_ticks() - t0;
            clean_ksps = timebase_ksps(nn, dt);
        }
        (void)capture_settle();
    }

    uint32_t ov[3], ticks[3] = { 0, 0, 0 };
    bool     ok[3];
    uint32_t late = 0, missed = 0;
    for (int l = 0; l < 3; l++) {
        ok[l] = sweep_point(halves, (enum sweep_load)l, &ticks[l]);
        ov[l] = dma_overrun;
        if (l == SWEEP_PROCESS) { late = late_service; missed = proc_missed; }
    }
    /* Measured rate of the idle run. */
    const uint32_t meas_ksps = ok[0] ? timebase_ksps(halves * capture_half_len(), ticks[0]) : 0u;
    /* Longest line: 150 characters plus NUL; every number is at most
     * 10 digits, "STOPPED" is shorter. */
    char line[208];
    char *p = copy_str(line, "postdiv ");      p = u32_to_str(p, st.p1);
    *p++ = '/';                                p = u32_to_str(p, st.p2);
    p = copy_str(p, "  adc clock Hz ");        p = u32_to_str(p, clock_adc_hz());
    p = copy_str(p, "  ksps nom ");            p = u32_to_str(p, capture_nominal_ksps(0u));
    p = copy_str(p, " clean ");                p = u32_to_str(p, clean_ksps);
    p = copy_str(p, " loaded ");               p = u32_to_str(p, meas_ksps);
    p = copy_str(p, "  overrun idle/process/sfr ");
    for (int l = 0; l < 3; l++) {
        if (l) { *p++ = '/'; }
        p = ok[l] ? u32_to_str(p, ov[l]) : copy_str(p, "STOPPED");
    }
    p = copy_str(p, "  late ");               p = u32_to_str(p, late);
    p = copy_str(p, "  missed ");             p = u32_to_str(p, missed);
    copy_str(p, "\r\n");
    console_puts(line);               /* blocking: works from main() too */
    return ok[SWEEP_PROCESS] && (ov[SWEEP_PROCESS] == 0u) && (missed == 0u) && (late == 0u);
}

void console_sweep(uint32_t halves, bool choose)
{
    uint32_t count = 0;
    const struct pll_step *steps = capture_sweep_steps(&count);  /* slowest rate first */
    const bool was_running = capture_running();

    console_kv("[sweep] halves per point", halves);
    console_puts("[sweep] back-to-back conversions; the rate is the ADC clock, PLL1 output dividers\r\n"
                 "[sweep] slowest rate first: the first row is the one the DMA should manage,\r\n"
                 "[sweep] so a failure there is the chain, not the rate\r\n"
                 "[sweep] idle = CPU polls RAM only, process = main-loop processing, sfr = CPU polls an SFR\r\n"
                 "[sweep] overrun must be 0 for a usable rate; late/missed are from the process run\r\n"
                 "[sweep] postdiv = PLL1 POSTDIV1/POSTDIV2; the adc clock is read back from the registers\r\n"
                 "[sweep] clean = rate of one burst with nothing else running; loaded = rate while\r\n"
                 "[sweep] the three runs below are going. Trust clean: the loaded figure is\r\n"
                 "[sweep] measured by a CPU drowning in overrun interrupts\r\n");

    /* Time base check: 100 ms of CPU time (200 MHz) must be 1 250 000
     * ticks. Anything else and the measured rates are off by the same
     * factor - and the assumption about the timer's clock is wrong. */
    console_kv("[sweep] timer check, ticks per 100 ms (expect 1250000)", timebase_check());

    struct pll_step best = { 0u, 0u };
    uint32_t best_ksps = 0u;              /* fastest clean row           */
    uint32_t clean = 0u, rows = 0u;
    for (uint32_t i = 0; i < count; i++) {
        console_puts("[sweep] ");
        rows++;
        if (sweep_row(steps[i], halves)) {
            clean++;
            const uint32_t k = capture_nominal_ksps(0u);
            if (k > best_ksps) { best = steps[i]; best_ksps = k; }
        }
    }
    console_kv("[sweep] rows", rows);
    console_kv("[sweep] rows with overrun 0, late 0 and missed 0", clean);

    /* The question the sweep answers: the highest rate at which the CPU
     * gets every half (missed 0) and the DMA every sample (overrun 0),
     * with the main-loop processing running. A rate with overruns also
     * raises the DMA interrupt for every lost sample (1.6 million per
     * second at 40 MSPS on the board) and starves the console. */
    if (best_ksps != 0u) {
        console_kv("[sweep] highest clean rate, ksps", best_ksps);
        console_kv("[sweep]   at POSTDIV1", best.p1);
        console_kv("[sweep]   at POSTDIV2", best.p2);
        if (choose) { (void)capture_set_pll(best.p1, best.p2); }
    } else {
        console_puts("[sweep] NO CLEAN RATE - every row lost samples or halves\r\n");
    }
    if (!choose) { (void)capture_set_pll(ADC_PLL_POSTDIV1, ADC_PLL_POSTDIV2); }
    counters_clear();
    if (was_running) { capture_start(); }
    console_puts("[sweep] done: counters cleared\r\n");
}

static void cmd_sweep_fn(int argc, char **argv)
{
    uint32_t halves = SWEEP_HALVES_DEFAULT;
    if ((argc > 2) || ((argc == 2) && !arg_u32(argv[1], 10u, 100000u, &halves))) {
        usage("sweep [halves per point 10..100000, default 2000]");
        return;
    }
    console_sweep(halves, false);
}
CMD_DEFINE(sweep, "sweep", cmd_sweep_fn, "sweep [halves] - overrun vs sample rate over the clock ladder");

static void cmd_clk_fn(int argc, char **argv)
{
    uint32_t v;
    if ((argc != 2) || !arg_u32(argv[1], 100u, 1000u, &v)) {
        usage("clk <100..1000>  (ADC clock divide ratio x 100: 100 = 320 MHz = 40 MSPS, 500 = 64 MHz = 8 MSPS, 1000 = 32 MHz = 4 MSPS, the slowest the ADC may run)");
        return;
    }
    const uint32_t rc = capture_set_clkdiv(v);
    put_kv("ratio asked for", v);
    put_kv("ratio read back", capture_clkdiv());
    put_kv("adc clock Hz", clock_adc_hz());
    put_kv("ksps nominal", capture_nominal_ksps(v));
    put_line(clock_adc_div_error(rc));
    if (rc != CLKDIV_OK) { cmd_parser_fail(); }
}
CMD_DEFINE(clk, "clk", cmd_clk_fn, "clk <100..1000> - CLKGEN6 divide ratio x 100 (does NOT change the rate)");

static void cmd_pll_fn(int argc, char **argv)
{
    uint32_t p1, p2;
    if ((argc != 3) || !arg_u32(argv[1], 1u, 7u, &p1) || !arg_u32(argv[2], 1u, 7u, &p2)) {
        usage("pll <postdiv1 1..7> <postdiv2 1..7>  (ADC clock = 1600 MHz / (p1*p2), p1 >= p2; 5 1 = 320 MHz = 40 MSPS, 5 5 = 64 MHz = 8 MSPS, 7 7 = 32.65 MHz = 4.08 MSPS)");
        return;
    }
    const uint32_t rc = capture_set_pll(p1, p2);
    put_kv("postdiv1", clock_adc_pll_postdiv1());
    put_kv("postdiv2", clock_adc_pll_postdiv2());
    put_kv("adc clock Hz", clock_adc_hz());
    put_kv("ksps nominal", capture_nominal_ksps(0u));
    put_line(clock_adc_div_error(rc));
    if (rc != CLKDIV_OK) { cmd_parser_fail(); }
}
CMD_DEFINE(pll, "pll", cmd_pll_fn, "pll <p1> <p2> - PLL1 output dividers = the sample rate");

/* ------------------------------------------------------------------ *
 * "test" - the parts of a run, one command
 *
 * The firmware does nothing on its own any more: it boots, brings the
 * console up and waits. The colleague at the board first proves that the
 * console works at all (anything typed echoes, "help" answers), then
 * runs the parts of the test from here. Every part ends in exactly one
 * PASS or FAIL line so that the log stays readable.
 *
 * Order in "test all", and the reason for it:
 *   self   is the chain intact?  ADC -> DMA -> RAM on the internal
 *          reference, at the slowest clock. If this fails nothing after
 *          it means anything, so "all" stops here and only here.
 *   clock  does a divider change arrive in the hardware? Every ratio is
 *          switched and read back, nothing is measured. This separates
 *          "the switch does not happen" from "the switch happens and the
 *          rate does not follow" - the question run 7 left open.
 *   sweep  where is the limit? The ladder from the slowest rate up, with
 *          overrun, late and missed per point. This is the table the
 *          customer gets.
 *   dac    does everything arrive, in order? The DAC2 triangle through
 *          the same chain. Counters cannot show a swapped or skipped
 *          half; a known signal can.
 * ------------------------------------------------------------------ */
#define TEST_SWEEP_HALVES   2000u
#define TEST_RATE_HALVES    200u
#define TEST_DAC_HALVES     64u

static bool test_self(void)
{
    uint32_t mean = 0;
    console_puts("[test] self: ADC -> DMA -> RAM on the internal 15/16 VDD reference\r\n");
    const uint32_t rc = capture_selftest(&mean);
    console_kv("[test]   mean (expect 3648..4032)", mean);
    console_puts((rc == 0u) ? "[test] self: PASS\r\n" : "[test] self: FAIL\r\n");
    if (rc == 6u)      { console_puts("[test]   no data - nothing converted\r\n"); }
    else if (rc == 7u) { console_puts("[test]   mean outside the window\r\n"); }
    else if (rc == 8u) { console_puts("[test]   the DMA channel switched itself off\r\n"); }
    return rc == 0u;
}

/* The CLKGEN6 divide ratios, for "test clock" only: this is the knob
 * that writes and reads back correctly and does not change the rate
 * (HARDWARE-LOG runs 8 and 9). The rate ladder lives in capture.c and is
 * made of PLL settings. Ratios between 100 and 200 are left out - FRACDIV
 * does nothing while INTDIV is 0, so they cannot be realised. */
static const uint32_t clkdiv_ratios[] = {
    1000u, 900u, 800u, 700u, 600u, 500u, 450u, 400u, 350u, 300u, 250u, 200u, 100u
};

static bool test_clock(void)
{
    const uint32_t count  = sizeof clkdiv_ratios / sizeof clkdiv_ratios[0];
    const uint32_t *ratios = clkdiv_ratios;
    uint32_t bad = 0u;
    console_puts("[test] clock: switch every divide ratio and read it back - no measurement\r\n"
                 "[test]   order per ratio: DMA down, ADC core off, CLKGEN6 off, divider\r\n"
                 "[test]   written and read back, generator on, DIVSWEN, CLKRDY, core on\r\n");
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t rc   = capture_set_clkdiv(ratios[i]);
        const uint32_t back = capture_clkdiv();
        /* Longest line: ratio and read-back (4 digits each), the clock in
         * Hz (9 digits) and the longest error text (about 50), well
         * inside 160. */
        char line[160];
        char *q = copy_str(line, "[test]   ratio ");  q = u32_to_str(q, ratios[i]);
        q = copy_str(q, " -> read back ");            q = u32_to_str(q, back);
        q = copy_str(q, ", adc clock Hz ");           q = u32_to_str(q, clock_adc_hz());
        q = copy_str(q, ", ");                        q = copy_str(q, clock_adc_div_error(rc));
        if ((rc == CLKDIV_OK) && (back != ratios[i])) {
            q = copy_str(q, " - BUT THE READBACK DIFFERS");
        }
        copy_str(q, "\r\n");
        console_puts(line);
        if ((rc != CLKDIV_OK) || (back != ratios[i])) { bad++; }
    }
    (void)capture_set_clkdiv(ADC_CLKDIV);
    console_kv("[test]   ratios that did not arrive", bad);
    console_puts("[test]   NOTE: this only proves the register holds the value. Runs 8 and 9\r\n"
                 "[test]   passed here and the rate did not follow at any ratio - the rate\r\n"
                 "[test]   is set with the PLL now (test sweep, pll command).\r\n");
    console_puts((bad == 0u) ? "[test] clock: PASS\r\n" : "[test] clock: FAIL\r\n");
    return bad == 0u;
}

static bool test_clkoff(void)
{
    /* Is CLKGEN6 the ADC's clock at all? Table 16-1 says so, and the
     * generator's divider has no effect on the rate - so ask the board. */
    console_puts("[test] clkoff: switch CLKGEN6 off and try to convert anyway\r\n"
                 "[test]   Table 16-1 names CLKGEN6 as the ADC clock, but its divider\r\n"
                 "[test]   changes nothing. If halves still arrive with the generator\r\n"
                 "[test]   off, the ADC is not running off it and that explains it all.\r\n");
    const uint32_t rc = capture_clkoff_probe(8u);
    console_kv("[clkoff]   halves after the generator was switched off", blocks_done);
    if (rc == 0u) {
        console_puts("[test] clkoff: THE ADC KEPT CONVERTING WITH CLKGEN6 OFF\r\n"
                     "[test]   -> CLKGEN6 is not (only) the ADC clock; the rate must come\r\n"
                     "[test]      from somewhere else. This is the finding, not a failure.\r\n");
        return false;
    }
    console_puts("[test] clkoff: no data with the generator off - CLKGEN6 does feed the ADC\r\n");
    return true;
}

static bool test_rate(uint32_t halves)
{
    uint32_t ksps = 0u;
    const uint32_t ratio   = capture_clkdiv();
    const uint32_t nominal = capture_nominal_ksps(ratio);
    console_puts("[test] rate: delivered rate at the ratio set now\r\n");
    console_kv("[test]   ratio", ratio);
    console_kv("[test]   ksps nominal", nominal);
    const uint32_t rc = capture_measure_rate(halves, &ksps);
    if (rc != 0u) {
        console_puts("[test]   no data\r\n[test] rate: FAIL\r\n");
        return false;
    }
    console_kv("[test]   ksps measured", ksps);
    console_kv("[test]   overrun during the measurement", dma_overrun);
    /* 10 % is the window the old rate test used; the rate comes from a
     * divided clock, so anything outside it means the divider is not
     * doing what the register says. */
    const uint32_t diff = (ksps > nominal) ? (ksps - nominal) : (nominal - ksps);
    const bool ok = (nominal != 0u) && (diff <= (nominal / 10u));
    console_puts(ok ? "[test] rate: PASS\r\n"
                    : "[test] rate: FAIL - the delivered rate does not follow the ratio\r\n");
    return ok;
}

static bool test_sweep(uint32_t halves)
{
    console_puts("[test] sweep: the ladder from the slowest rate up\r\n");
    console_sweep(halves, false);
    console_puts("[test] sweep: done - read the table, there is no single verdict\r\n");
    return true;
}

static bool test_dac(uint32_t halves)
{
    console_puts("[test] dac: the DAC2 triangle through ADC, DMA and the ping-pong buffer\r\n");
    /* SLPDAT is the step per DAC clock, so a LARGER value is a FASTER
     * triangle. 8 leaves the signal almost standing still inside one
     * captured buffer - run 12 measured a swing of 72 counts and the test
     * could say nothing. 64 puts a full period in the window, which is
     * what run 13 then showed as a triangle. */
    if (!dac_running(2u) && !dac_triangle_start(2u, 0x100u, 0xF00u, 64u)) {
        console_puts("[test]   CLKGEN7 did not come up - DAC2 is off\r\n[test] dac: FAIL\r\n");
        return false;
    }
    const uint32_t rc = run_dactest(halves);
    console_puts((rc == 0u) ? "[test] dac: PASS\r\n" : "[test] dac: FAIL\r\n");
    return rc == 0u;
}

/* ------------------------------------------------------------------ *
 * "test matrix" - every documented way to set the sample rate, tried
 *
 * Four of these were tried before and written off as "does not work".
 * For three of them the reason turned out to be in our own code: the ADC
 * trigger number selected a different SCCP module, the auxiliary output
 * carried the timer rollover instead of the special event trigger, and
 * the trigger module was clocked from a different PLL than the ADC. Since
 * the documentation has been wrong about this device twice, every
 * combination is asked of the board rather than reasoned about.
 *
 * Each variant answers the same four questions, with the instruments that
 * have earned trust:
 *   converts      does a burst produce halves at all - bounded, so a
 *                 variant that does nothing costs milliseconds
 *   rate follows  the delivered rate from ONE CLEAN BURST timed with
 *                 Timer1, never under load. This is where eleven runs
 *                 went wrong: measured while the CPU drowned in the
 *                 overrun interrupt, every rate came out ten times high
 *   xfer/trigger  transfers per trigger = 2048 / (window / trigger
 *                 period). Microchip acknowledges "a few transfers are
 *                 possible per one trigger" on this silicon; this is the
 *                 direct measurement of it
 *   data intact   the DAC triangle through the chain, for the variants
 *                 that got that far - run last and only for those, to
 *                 keep the log readable
 * ------------------------------------------------------------------ */
#define MATRIX_POINTS   3u
static const uint32_t matrix_ksps[MATRIX_POINTS] = { 4000u, 8000u, 20000u };
#define MATRIX_TOL_PCT  10u

struct matrix_result {
    bool converts;
    bool rate_follows;
    bool checked;
};

/* One rate point: select, run one clean burst, report. */
static bool matrix_point(capture_variant_t v, uint32_t want, bool show_regs)
{
    if (!capture_select_variant(v, want)) {
        console_kv("[matrix]   could not configure for ksps", want);
        return false;
    }
    if (show_regs) { capture_variant_regs(); }

    const uint32_t nominal = capture_variant_ksps();
    const uint32_t n       = 2u * capture_half_len();
    const uint32_t t0      = timebase_ticks();
    const uint32_t rc      = capture_oneshot();
    const uint32_t ticks   = timebase_ticks() - t0;
    (void)capture_settle();

    if (rc != 0u) {
        console_kv("[matrix]   ksps asked", want);
        console_puts("[matrix]     NO DATA - nothing converted\r\n");
        return false;
    }
    const uint32_t ksps = timebase_ksps(n, ticks);
    const uint32_t diff = (ksps > nominal) ? (ksps - nominal) : (nominal - ksps);
    const bool     ok   = (nominal != 0u) && (diff <= (nominal * MATRIX_TOL_PCT / 100u));

    /* Longest line: four numbers of at most 10 digits and about 70
     * characters of text, well inside 160. */
    char line[160];
    char *q = copy_str(line, "[matrix]   asked ");  q = u32_to_str(q, want);
    q = copy_str(q, "  nominal ");                  q = u32_to_str(q, nominal);
    q = copy_str(q, "  measured ");                 q = u32_to_str(q, ksps);
    q = copy_str(q, " ksps  -> ");
    q = copy_str(q, ok ? "follows" : "DOES NOT FOLLOW");
    copy_str(q, "\r\n");
    console_puts(line);

    /* Transfers per trigger, for the triggered variants: the trigger
     * period is exact, the transfer count is exactly one buffer, and the
     * window came from Timer1. */
    const uint32_t trig_ns = capture_trigger_period_ns();
    if ((trig_ns != 0u) && (ticks != 0u)) {
        const uint32_t window_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / TIMEBASE_HZ);
        const uint32_t triggers  = window_ns / trig_ns;
        if (triggers != 0u) {
            console_kv("[matrix]     triggers in the window", triggers);
            console_kv("[matrix]     transfers per trigger x100", (n * 100u) / triggers);
        }
    }
    console_kv("[matrix]     overrun during the burst", dma_overrun);
    return ok;
}

static void cmd_matrix(void)
{
    struct matrix_result res[CAP_VAR_COUNT];
    console_puts("\r\n[matrix] every documented way to set the sample rate, tried on the board\r\n"
                 "[matrix] rate measured on one clean burst per point, never under load\r\n"
                 "[matrix] see sccp.h for the three errors this replaces\r\n");

    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        res[v].converts = false;
        res[v].rate_follows = false;
        res[v].checked = false;

        console_puts("\r\n[matrix] ");
        console_puts(capture_variant_name((capture_variant_t)v));
        console_puts("\r\n");

        uint32_t good = 0u;
        for (uint32_t i = 0; i < MATRIX_POINTS; i++) {
            if (matrix_point((capture_variant_t)v, matrix_ksps[i], i == 0u)) { good++; }
            if (blocks_done != 0u) { res[v].converts = true; }
        }
        res[v].rate_follows = (good == MATRIX_POINTS);
        console_puts(res[v].rate_follows
                     ? "[matrix]   VERDICT: the rate follows at every point\r\n"
                     : "[matrix]   VERDICT: not usable as a rate control\r\n");
    }

    /* The data check, only for what survived - one DAC capture each, so
     * the log stays readable. */
    console_puts("\r\n[matrix] data check on the variants whose rate followed\r\n");
    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        if (!res[v].rate_follows) { continue; }
        console_puts("[matrix] ");
        console_puts(capture_variant_name((capture_variant_t)v));
        console_puts("\r\n");
        if (capture_select_variant((capture_variant_t)v, 8000u)) {
            res[v].checked = (run_dactest(0u) == 0u);
        }
    }

    console_puts("\r\n[matrix] SUMMARY\r\n");
    for (uint32_t v = 0; v < (uint32_t)CAP_VAR_COUNT; v++) {
        char line[160];
        char *q = copy_str(line, "[matrix]   ");
        q = copy_str(q, res[v].rate_follows ? "RATE OK  " : "         ");
        q = copy_str(q, res[v].checked      ? "DATA OK  " : "         ");
        q = copy_str(q, capture_variant_name((capture_variant_t)v));
        copy_str(q, "\r\n");
        console_puts(line);
    }
    /* Back to a known state: the boot setting. */
    (void)capture_select_variant(CAP_VAR_B2B, capture_nominal_ksps(0u));
    (void)capture_set_pll(ADC_PLL_POSTDIV1, ADC_PLL_POSTDIV2);
    console_puts("[matrix] done - back at the boot setting\r\n");
}

static void test_list(void)
{
    put_line("test all   [halves]  - self, clock, sweep, dac in that order");
    put_line("test self            - ADC -> DMA -> RAM on the internal reference");
    put_line("test clock           - switch every CLKGEN6 ratio and read it back");
    put_line("test clkoff          - switch CLKGEN6 off: does the ADC still convert?");
    put_line("test matrix          - every way to set the rate, tried and measured");
    put_line("test rate  [halves]  - delivered rate at the ratio set now");
    put_line("test sweep [halves]  - the rate ladder, slowest first, with the counters");
    put_line("test dac   [halves]  - the DAC2 triangle: is everything there, in order?");
    put_line("pll <p1> <p2> sets the rate by hand; clk sets the (ineffective) CLKGEN6");
    put_line("ratio; regs prints the registers");
}

static void cmd_test_fn(int argc, char **argv)
{
    uint32_t halves = 0u;                  /* 0 = the part's own default */
    if (argc == 1) { test_list(); return; }
    if (argc > 3) { usage("test <all|self|clock|clkoff|matrix|rate|sweep|dac> [halves]"); return; }
    if ((argc == 3) && !arg_u32(argv[2], 1u, 100000u, &halves)) {
        usage("test <all|self|clock|clkoff|matrix|rate|sweep|dac> [halves]");
        return;
    }
    const char *what = argv[1];

    if (strcmp(what, "self") == 0) {
        if (!test_self()) { cmd_parser_fail(); }
    } else if (strcmp(what, "clock") == 0) {
        if (!test_clock()) { cmd_parser_fail(); }
    } else if (strcmp(what, "clkoff") == 0) {
        if (!test_clkoff()) { cmd_parser_fail(); }
    } else if (strcmp(what, "matrix") == 0) {
        cmd_matrix();
    } else if (strcmp(what, "rate") == 0) {
        if (!test_rate((halves != 0u) ? halves : TEST_RATE_HALVES)) { cmd_parser_fail(); }
    } else if (strcmp(what, "sweep") == 0) {
        (void)test_sweep((halves != 0u) ? halves : TEST_SWEEP_HALVES);
    } else if (strcmp(what, "dac") == 0) {
        if (!test_dac((halves != 0u) ? halves : TEST_DAC_HALVES)) { cmd_parser_fail(); }
    } else if (strcmp(what, "all") == 0) {
        console_puts("\r\n[test] ALL - self, clock, sweep, dac\r\n");
        (void)capture_set_clkdiv(ADC_CLKDIV);      /* start slow          */
        const bool self_ok = test_self();
        if (!self_ok) {
            /* The only part whose failure stops the rest: without a
             * working chain every number after it is meaningless. */
            console_puts("[test] ALL: STOPPED - the chain itself does not work\r\n");
            cmd_parser_fail();
            return;
        }
        const bool clock_ok = test_clock();
        (void)test_clkoff();
        (void)test_sweep((halves != 0u) ? halves : TEST_SWEEP_HALVES);
        const bool dac_ok = test_dac(TEST_DAC_HALVES);
        console_puts("\r\n[test] ALL DONE\r\n");
        console_puts(self_ok  ? "[test]   self:  PASS\r\n" : "[test]   self:  FAIL\r\n");
        console_puts(clock_ok ? "[test]   clock: PASS\r\n" : "[test]   clock: FAIL\r\n");
        console_puts("[test]   sweep: see the table above\r\n");
        console_puts(dac_ok   ? "[test]   dac:   PASS\r\n" : "[test]   dac:   FAIL\r\n");
        if (!clock_ok || !dac_ok) { cmd_parser_fail(); }
    } else {
        test_list();
        cmd_parser_fail();
    }
}
CMD_DEFINE(test, "test", cmd_test_fn, "test [all|self|clock|clkoff|matrix|rate|sweep|dac] [halves]");

static void cmd_snap_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* One buffer, and the DMA interrupt ends the stream itself. This is
     * the only way to get a coherent window at a rate where the main loop
     * cannot keep up: "start, wait, stop, dump" reads a half that the DMA
     * has overwritten a thousand times in the meantime, and at a rate
     * with overruns the "stop" never even arrives, because the console's
     * receive interrupt sits below the DMA channel. Afterwards the whole
     * buffer stands still and "dump" can read all of it. */
    const uint32_t n     = 2u * capture_half_len();
    const uint32_t t0    = timebase_ticks();
    const uint32_t rc    = capture_oneshot();
    const uint32_t ticks = timebase_ticks() - t0;
    (void)capture_settle();

    if (rc != 0u) {
        put_line(capture_overrun_aborted()
                 ? "snap: stopped by the overrun brake - this rate floods the CPU"
                 : "snap: no data");
        cmd_parser_fail();
        return;
    }
    const uint32_t window_ns = (uint32_t)(((uint64_t)ticks * 1000000000ull) / TIMEBASE_HZ);
    put_kv("samples", n);
    put_kv("window ns", window_ns);
    put_kv("ksps measured", timebase_ksps(n, ticks));
    put_kv("ksps nominal", capture_nominal_ksps(0u));
    put_kv("overrun during the burst", dma_overrun);
    put_kv("input", capture_pinsel());
    put_kv("adc core", adc_core());
}
CMD_DEFINE(snap, "snap", cmd_snap_fn, "snap - fill the buffer once and stop; then dump it");

static void cmd_rate_fn(int argc, char **argv)
{
    uint32_t want = 0u, got = 0u;
    if ((argc != 2) || !arg_u32(argv[1], 4000u, 40000u, &want)) {
        usage("rate <4000..40000 ksps>  (the closest the PLL can make; 'rate 8000' gives exactly 8000)");
        return;
    }
    const uint32_t rc = capture_set_rate(want, &got);
    put_kv("ksps asked for", want);
    put_kv("ksps set", got);
    put_kv("pll1 fbdiv", clock_pll1_fbdiv());
    put_kv("pll1 postdiv1", clock_adc_pll_postdiv1());
    put_kv("pll1 postdiv2", clock_adc_pll_postdiv2());
    put_kv("adc clock Hz", clock_adc_hz());
    put_line(clock_adc_div_error(rc));
    if (rc != CLKDIV_OK) { cmd_parser_fail(); }
}
CMD_DEFINE(rate, "rate", cmd_rate_fn, "rate <ksps> - the sample rate, 4000..40000");

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
        console_trace("[boot] uart reclocked to PLL2, 115200 8N1\r\n");
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
    (void)cmd_register(&cmd_sweep);
    (void)cmd_register(&cmd_clk);
    (void)cmd_register(&cmd_pll);
    (void)cmd_register(&cmd_test);
    (void)cmd_register(&cmd_core);
    (void)cmd_register(&cmd_buf);
    (void)cmd_register(&cmd_dac);
    (void)cmd_register(&cmd_dactest);
    (void)cmd_register(&cmd_snap);
    (void)cmd_register(&cmd_rate);
    (void)cmd_register(&cmd_reset);

    /* Banner, once at start-up. A human sees what is talking and which
     * build it is; a script simply reads on until the readiness byte that
     * follows the first prompt. */
    console_puts("\r\n"
                 "adc_dma_40msps - ADC at 40 MSPS into RAM via DMA\r\n"
                 SIM_BANNER_NOTE           /* empty on silicon            */
                 "board: " BOARD_NAME "\r\n"
                 "build: " BUILD_ID "\r\n"
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

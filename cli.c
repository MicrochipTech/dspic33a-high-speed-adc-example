/*
 * cli.c
 *
 * Command console for the ADC/DMA example, built on cmd_parser.c
 * (https://github.com/zabooh/cmd_parser, Apache 2.0). This file owns the
 * transport and the commands; the parser itself knows no hardware.
 *
 * Transport on the EV74H48A
 *   UART1 on the PKOB4 USB-UART channel: U1TX -> RH0 (RP113, DIM pin
 *   P102 "UART_PKoB_TX"), U1RX <- RD10 (RP59, DIM pin P100
 *   "UART_PKoB_RX"), 115200 8N1. The channel shares the USB cable with
 *   the debugger and shows up on the PC as a COM port (user guide
 *   DS70005562D 2.1.2). The PPS codes are the ones Microchip's own
 *   example uses on this board (U1TX = 19, Table "Output Selection for
 *   Remappable Pins", p613).
 *
 * The parser as its own thread
 *   Received bytes are handled in the UART1 receive interrupt, which
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
#include "adc_dma_40msps.h"
#include "cmd_parser.h"

/* ------------------------------------------------------------------ *
 * UART1 transport
 * ------------------------------------------------------------------ */

/* Baud rate. UART clock = Standard Speed Peripheral Clock, 100 MHz
 * (1:2 of the 200 MHz CPU clock). CLKMOD = 1 selects the fractional
 * baud generator, where BRG = F_clk / baud (no -1): 100 000 000 / 115 200
 * = 868 -> 115 207 baud, the value MCC generates for this board. */
#define UART_BRG          868u

#define UART_RX_PRIORITY  1u      /* below the DMA interrupt (4)        */

static void uart1_init(void)
{
    /* Pins: RH0 = U1TX (output), RD10 = U1RX (input). Neither port has an
     * analog function. Peripheral pin select needs IOLOCK cleared. */
    TRISHbits.TRISH0 = 0u;
    TRISDbits.TRISD10 = 1u;
    RPCONbits.IOLOCK = 0u;
    _U1RXR  = 59u;                    /* RP59  -> U1RX                  */
    _RP113R = 19u;                    /* RP113 <- U1TX                  */
    RPCONbits.IOLOCK = 1u;

    U1CON = 0u;
    U1CONbits.CLKMOD = 1u;            /* fractional baud generator      */
    U1CONbits.CLKSEL = 0u;            /* standard speed peripheral clock*/
    U1CONbits.MODE   = 0u;            /* 8-bit, no parity               */
    U1CONbits.STP    = 0u;            /* one stop bit                   */
    U1BRG = UART_BRG;
    U1STAT = 0u;                      /* RXWM = 0: IRQ on one byte      */
    U1CONbits.ON   = 1u;
    U1CONbits.TXEN = 1u;
    U1CONbits.RXEN = 1u;

    /* Receive interrupt: IRQ 98, IEC3/IFS3 bit 2, priority in IPC12. */
    IPC12bits.U1RXIP = UART_RX_PRIORITY;
    IFS3bits.U1RXIF  = 0u;
    IEC3bits.U1RXIE  = 1u;
}

/* Output sink for the parser: take what fits into the transmit FIFO and
 * report how much that was. The parser re-offers the rest (see
 * cmd_parser.h, "Flow control"). */
static size_t console_write(const char *data, size_t len)
{
    size_t n = 0;
    while ((n < len) && !U1STATbits.TXBF) {
        U1TXB = (uint8_t)data[n++];
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
    while (!U1STATbits.RXBE) {
        if ((uint8_t)U1RXB == 0x03u) {
            cmd_parser_abort();
        }
    }
}

/* The parser thread: every received byte goes to the line editor, and
 * a completed line is dispatched right here, in interrupt context. */
void __attribute__((interrupt, no_auto_psv)) _U1RXInterrupt(void)
{
    while (!U1STATbits.RXBE) {
        cmd_parser_feed_char((char)U1RXB);
    }
    IFS3bits.U1RXIF = 0u;
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

static void put_kv(const char *key, uint32_t v)
{
    char line[48];
    char *p = line;
    while (*key) { *p++ = *key++; }
    *p++ = ':'; *p++ = ' ';
    p = u32_to_str(p, v);
    *p++ = '\r'; *p++ = '\n'; *p = '\0';
    cmd_parser_write(line);
}

static void put_line(const char *s)
{
    cmd_parser_write(s);
    cmd_parser_write("\r\n");
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
        *p++ = '\r'; *p++ = '\n'; *p = '\0';
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
    while (!U1STATbits.TXMTIF) { }      /* let the reply leave first */
    __asm__ volatile ("reset");
}
CMD_DEFINE(reset, "reset", cmd_reset_fn, "reset - software reset");

/* ------------------------------------------------------------------ */
void cli_init(void)
{
    uart1_init();
    cmd_parser_init(console_write);
    cmd_parser_set_yield(console_yield);     /* after init - init clears it */
    (void)cmd_register(&cmd_version);
    (void)cmd_register(&cmd_status);
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
    cmd_parser_write("\r\n"
                     "adc_dma_40msps - ADC at 40 MSPS into RAM via DMA\r\n"
                     "board: EV74H48A, dsPIC33AK512MPS512 GP DIM\r\n"
                     "build: " __DATE__ " " __TIME__ "\r\n"
                     "type 'help' for the commands\r\n");

    cmd_parser_prompt();                     /* sync point for a reader */
}

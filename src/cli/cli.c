/*
 * cli.c
 *
 * Console for the ADC/DMA example: the command parser from
 * https://github.com/zabooh/cmd_parser (Apache 2.0), the commands, and
 * the console's three output framings, all built on the UART transport
 * driver (uart.c, since P5.1/P5.2, 27.09.2026) - blocking trace output
 * (console_puts()), the parser's own non-blocking sink (console_write()),
 * and the binary block transfer's abortable one (console_write_raw()).
 * This file touches no UART register any more; uart.c owns the pins, the
 * PPS routing, the baud generator and the receive interrupt - see its
 * header comment for the wiring and why 115200 8N1. The parser itself
 * knows no hardware.
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
 *   Received bytes are handled in uart.c's receive interrupt, which
 *   calls uart_rx_hook() (below) at priority 1. A command executes
 *   inside that interrupt, including its output; the DMA interrupt
 *   (priority 4) preempts it, so the measurement keeps running while a
 *   long reply drains. main() is the one that waits during a long reply -
 *   it only processes buffer halves and blinks, and it reports that with
 *   proc_missed if it matters. Ctrl+C aborts a long reply: the yield
 *   hook, which runs while the transmit buffer is full, peeks at the
 *   receiver for it.
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
 *   stream on <ksps> | off     the chain as a standing stream, main() processing
 *   stream                     its state: rate, halves, overrun/late/missed, load
 *   chain all|from n|n|run     the chain test SCCP1 -> ADC -> DMA -> CPU
 *                              (chaintest.c, docs/CHAIN-TEST-PLAN.md)
 *   reset                      software reset
 *
 * Every command ends its output with a newline and calls
 * cmd_parser_fail() on a usage or argument error, so a script sees NAK
 * (see cmd_parser.h, "Prompt as a protocol element").
 */

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
#include "chaintest.h"
#include "bench.h"
#include "led.h"
#include "diag.h"
#include "console.h"
#include "sim.h"
#include "cmd_parser.h"
#include "fmt.h"
#include "stats.h"
#include "uart.h"
#include "gui_link.h"
#include "routing.h"
#include "siggen.h"     /* SG.4: the "siggen" command */
#include "dma_tx.h"     /* SG.4: dma1_regs_visit() for "siggen regs" */
#include "sccp.h"       /* SG.4: sccp2_regs_visit() for "siggen regs" */

/* ------------------------------------------------------------------ *
 * UART transport - uart.c owns every register; this file only decides
 * WHICH baud divisor is right for the clock the CPU happens to be on
 * (clock_cpu_on_pll(), clock.h) and wires uart.c's primitives into the
 * console's three output framings and the parser's receive callback.
 * See uart.h for the pins, the PPS routing and why 115200 8N1.
 * ------------------------------------------------------------------ */

/* The two baud-rate-generator divisors this console ever asks uart.c
 * for; see uart.h's uart_cfg_t comment for how a divisor is computed.
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

void console_early_init(void)
{
    const uart_cfg_t cfg = { .brg = UART_BRG_FRC };
    uart_init(&cfg);
    console_puts("\r\n[boot] uart up on FRC, 115200 8N1\r\n");
}

/* Blocking output, usable at any time after console_early_init(): from
 * main(), from fail(), from _DefaultInterrupt() and from the receive
 * interrupt (the parser's own output goes through console_write()
 * below instead). Never blocks forever - see uart.h's
 * UART_TX_WAIT_LIMIT: console_puts() is called from _DefaultInterrupt()
 * to report a trap, and if the UART is not actually transmitting (wrong
 * baud divider, ON bit cleared, clock gone) an unbounded wait would
 * silently swallow the one message that explains the fault. Better a
 * garbled line than none. */
void console_puts(const char *s)
{
    while (*s) {
        uint32_t n = UART_TX_WAIT_LIMIT;
        while (uart_tx_full() && (--n != 0u)) { }
        uart_putc((uint8_t)*s++);
    }
}

void console_flush(void)
{
    uart_flush();
}

/* Bring the console back up from scratch, assuming nothing about the
 * current state of the pins, the PPS mapping or the UART (uart_reinit()).
 *
 * This is what _DefaultInterrupt() calls before it reports a trap. A trap
 * can have happened anywhere, including inside clock_init() or after some
 * other code disturbed the peripheral, and in that situation
 * console_sync_baud() is not enough: it only fixes the baud divider, and
 * it trusts clock.c to say what the clock is. Here the routing is written
 * again and the baud rate is picked from the clock the CPU is actually
 * on, so the one message that explains the fault has the best chance of
 * getting out. Safe to call when the console is already up, and from
 * interrupt context (uart_reinit() only waits bounded). */
void console_force_up(void)
{
    uart_reinit(clock_cpu_on_pll() ? UART_BRG_PLL : UART_BRG_FRC);
}

/* Make the baud generator match whatever clock the CPU is on right now.
 * fail() calls this first: a failure after the switch to PLL2 but before
 * cli_init() would otherwise print at the wrong rate. */
void console_sync_baud(void)
{
    (void)uart_set_baud(clock_cpu_on_pll() ? UART_BRG_PLL : UART_BRG_FRC);
}

/* Output sink for the parser: take what fits into the transmit FIFO and
 * report how much that was. The parser re-offers the rest (see
 * cmd_parser.h, "Flow control"). */
static void console_yield(void);

static size_t console_write(const char *data, size_t len)
{
    return uart_write((const uint8_t *)data, len);
}

/* Bytes that are not a string (console.h). The wait for FIFO space is
 * the same one the parser does, and it looks for Ctrl+C in the same
 * place - a 4 KB block takes 360 ms at 115200 baud and must stay
 * abortable. An abort ends the block early and the caller finishes the
 * frame anyway, so the client sees a short read, not silence. */
size_t console_write_raw(const uint8_t *data, size_t len)
{
    size_t n = 0;
    while (n < len) {
        if (uart_tx_full()) {
            console_yield();          /* FIFO full: drain, watch Ctrl+C */
            if (cmd_parser_aborted()) { break; }
            continue;
        }
        uart_putc(data[n++]);
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
    while (!uart_rx_empty()) {
        if (uart_getc() == 0x03u) {
            cmd_parser_abort();
        }
    }
}

/* Receive diagnostics, shown in the [stat] line and by "status": how many
 * bytes the interrupt took from the UART, the last one, and how many
 * CR / LF among them. "I type and nothing happens" is then one of three
 * things: rx stays 0 (nothing reaches the console's RX pin, the echo was
 * the terminal's), rx counts but cr stays 0 (the terminal sends LF only -
 * the parser ends a line on CR), or cr counts and still no reply (the
 * parser or the transmit path). */
static volatile uint32_t rx_count = 0;
static volatile uint32_t rx_cr    = 0;
static volatile uint32_t rx_lf    = 0;
static volatile uint8_t  rx_last  = 0;

/* The parser thread: every received byte goes to the line editor, and a
 * completed line is dispatched right here - called from uart.c's receive
 * interrupt (moved there in P5.1), in interrupt context, with the flag
 * already cleared for the byte that follows. */
void uart_rx_hook(uint8_t b)
{
    rx_count++;
    rx_last = b;
    if (b == 0x0Du)      { rx_cr++; }
    else if (b == 0x0Au) { rx_lf++; }
    cmd_parser_feed_char((char)b);
}

/* The small formatting helpers u32_to_str(), u32_to_hex() and copy_str()
 * (no printf, so the reply cost is predictable) live in lib/fmt.c since
 * P2.1 (27.09.2026), where the host test reaches them. */

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

/* diag.c's regs_dump() calls this, not uart_regs_visit() directly: cli.c
 * (and with it uart.c's caller) is not linked into the register-trace
 * harness (tests/trace/README.md, decision 1 of 26.09.2026), so keeping
 * the indirection here means the "regs" golden trace still gets the
 * harness's stub line for this section, unchanged, exactly as before
 * P5.1/P5.2 - diag.c and its .sources are untouched. reg_print (diag.h)
 * reproduces console_kv_hex()/console_puts() character for character
 * (port/regs.h, P4.8). */
void console_regs_dump(void)
{
    uart_regs_visit(reg_print);
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

/* Not static: src/tests/bench.c's moved test/matrix/sweep commands reply
 * through the same parser sink, so it and put_line()/arg_u32()/usage()
 * below are declared extern there instead of duplicated. */
void put_kv(const char *key, uint32_t v)
{
    char num[16];
    char *p = u32_to_str(num, v);
    copy_str(p, "\r\n");
    cmd_parser_write(key);
    cmd_parser_write(": ");
    cmd_parser_write(num);
}

void put_line(const char *s)
{
    cmd_parser_write(s);
    cmd_parser_write("\r\n");
}

/* put_kv()'s string counterpart, for "route list" (P11.5, 27.09.2026): a
 * route's src/sink are named by a short, static string (routing.c's
 * route_src_name()/route_sink_name()), not a number - no local buffer
 * needed, every argument is already a complete, nul-terminated string.
 * Static: only cmd_route_fn() below uses it, unlike put_kv()/put_line()
 * which bench.c and gui_link.c also borrow. */
static void put_kv_str(const char *key, const char *s)
{
    cmd_parser_write(key);
    cmd_parser_write(": ");
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
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
    char *end;
    unsigned long v;
    if (*s == '\0') { return false; }
    v = strtoul(s, &end, 0);
    if ((*end != '\0') || (v < lo) || (v > hi)) { return false; }
    *out = (uint32_t)v;
    return true;
}

void usage(const char *text)
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
    /* The relation that decides where the lost samples come from: one
     * burst is a whole buffer, so HALF fires once and DONE once, and
     * blocks must be exactly twice bursts. A larger factor means the
     * handler books the same event repeatedly; the factor itself is the
     * measurement. Read per capture, no sweep needed. */
    put_kv("bursts", burst_starts);
    put_kv("blocks (must be 2x bursts)", blocks_done);
    put_kv("isr_entries", isr_entries);
    put_kv("half_events", half_events);
    put_kv("done_events", done_events);
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

    /* BR.6 (27.09.2026), appended so an older parser of this reply still
     * works: no fixed-size line buffer here to size from the longest
     * line, unlike console_status_line()'s single buffer - put_kv() sends
     * each field through cmd_parser_write() on its own. */
    /* stack_used is a depth in bytes, stack_hwm_addr the address it reached
     * - two keys so neither can be misread as the other (both measured from
     * main()'s entry SP, where the paint starts; diag.h). */
    put_kv("stack_size", diag_stack_size());
    put_kv("stack_used", diag_stack_used_bytes());
    put_kv("stack_hwm_addr", diag_stack_used_max());
    put_kv("stack_free_pct", diag_stack_free_pct());
    /* Buffer placement, after relinking: address, its alignment (dma.c's
     * dma0_init() refuses a destination address that is not a multiple of
     * 4 - the DMA writes through a 32-bit path, "first % 4u != 0u"), the
     * length of the window currently armed (2 * half_len samples of 2
     * bytes each, the same product capture_init() passes to dma0_init()),
     * and whether the guard words behind it (capture.c) are still what
     * capture_init() wrote. */
    put_kv("buf_addr", (uint32_t)(uintptr_t)capture_buffer());
    put_kv("buf_align_mod4", (uint32_t)(uintptr_t)capture_buffer() % 4u);
    put_kv("buf_len", 2u * capture_half_len() * (uint32_t)sizeof(uint16_t));
    put_kv("buf_guard_ok", capture_guard_ok() ? 1u : 0u);
    /* Trap/reset history not already in the boot banner: diag_report_reset()
     * decodes and clears RCON there, and trap_seen/trap_vec/the boot stage
     * it happened at are printed there too, once, right after boot - so
     * neither RCON nor that one-shot report is repeated here (RCON reads
     * back 0 by the time any command runs). What follows are the current
     * values, which is what changes if a chain run hangs or a trap happens
     * later in the same session. */
    put_kv("boot_stage", boot_stage);
    put_kv("trap_seen", trap_seen);
    put_kv("trap_vec", trap_vec);
    put_kv("chain_mark", chain_mark);
}
CMD_DEFINE(status, "status", cmd_status_fn, "status - run state and counters");

static void cmd_regs_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    regs_dump();
}
CMD_DEFINE(regs, "regs", cmd_regs_fn, "regs - clock, ADC, DMA and UART registers");

/* routing.c's route_visit_t callback: turns every field routing_visit()
 * hands out into the same "key: value" line every other command's reply
 * already uses (put_kv()/put_line()) - see routing.h's route_vis_fmt_t. */
static void route_print(const char *name, uint32_t v, const char *s, route_vis_fmt_t fmt)
{
    switch (fmt) {
    case ROUTE_VIS_NUM:  put_kv(name, v);  break;
    case ROUTE_VIS_STR:  put_kv_str(name, s); break;
    case ROUTE_VIS_LINE: put_line(name); break;
    }
}

/* "route list" - the only sub-command today, dispatched inside this one
 * function exactly like "stream on|off|grab" (cmd_stream_fn() above): a
 * later sub-command (docs/DESIGN-MULTICHANNEL.md's other sinks, once they
 * read something) needs no new parser slot, only another branch here. */
static void cmd_route_fn(int argc, char **argv)
{
    static const char use[] = "route list - the active route(s) and the resource table";
    if ((argc == 2) && (strcmp(argv[1], "list") == 0)) {
        routing_visit(route_print);
        return;
    }
    usage(use);
}
CMD_DEFINE(route, "route", cmd_route_fn, "route list - the active route(s) and the resource table");

/* ------------------------------------------------------------------ *
 * "siggen" - the signal generator (SG.4, 29.09.2026): siggen.c plays a
 * wavegen table through DMA channel 1 into DAC1/DAC2, paced by SCCP2.
 *   siggen set <param> <value>    f0 h2..h7 decay amp lo hi, one per line
 *                                 (the 64-character line holds no more)
 *   siggen on <dac> <n> <play_hz> [snap] [force] [oc]
 *   siggen off | siggen regs | siggen   (status)
 * One parser slot; everything else is a sub-command, as for "stream".
 * Longest reply line: "transfers_per_s: " + 10 digits + CRLF = 29, and a
 * DEC value is at most 21 characters (fmt.h) - put_kv()'s own 16-byte
 * buffer is not used for those, dec_str[28] is.
 * ------------------------------------------------------------------ */
static void siggen_print(const char *name, int64_t v, siggen_vis_fmt_t fmt)
{
    char num[28];
    switch (fmt) {
    case SIGGEN_VIS_NUM: put_kv(name, (uint32_t)v); break;
    case SIGGEN_VIS_HEX: (void)u32_to_hex(num, (uint32_t)v); put_kv_str(name, num); break;
    case SIGGEN_VIS_DEC: (void)dec_to_str(num, v); put_kv_str(name, num); break;
    }
}

static void siggen_refused(siggen_result_t r)
{
    cmd_parser_write("siggen: ");
    put_line(siggen_result_name(r));
    if (r == SIGGEN_E_WAVEGEN) { put_kv("wavegen_err", siggen_wavegen_err()); }
    if (r == SIGGEN_E_ROUTE)   { put_kv("route_err", siggen_route_err()); }
    cmd_parser_fail();
}

static void cmd_siggen_fn(int argc, char **argv)
{
    static const char use[] =
        "siggen set <f0|h2..h7|decay|amp|lo|hi> <value> | siggen on <dac 1|2> <n 2..8192> "
        "<play_hz 100..1000000> [snap] [force] [oc] | siggen off | siggen regs | siggen";
    if (argc == 1) { siggen_visit(siggen_print); return; }
    if ((argc == 4) && (strcmp(argv[1], "set") == 0)) {
        int64_t v = 0;
        if (!fmt_parse_dec(argv[3], &v)) { usage(use); return; }
        const siggen_result_t r = siggen_set(argv[2], v);
        if (r != SIGGEN_OK) { siggen_refused(r); return; }
        put_kv_str(argv[2], argv[3]);
        return;
    }
    if ((argc == 2) && (strcmp(argv[1], "off") == 0)) {
        siggen_stop();
        put_line("siggen: off");
        return;
    }
    if ((argc == 2) && (strcmp(argv[1], "regs") == 0)) {
        dma1_regs_visit(reg_print);
        sccp2_regs_visit(reg_print);
        return;
    }
    uint32_t dac = 0u, n = 0u, hz = 0u;
    if ((argc >= 5) && (argc <= 8) && (strcmp(argv[1], "on") == 0) &&
        arg_u32(argv[2], 1u, 2u, &dac) && arg_u32(argv[3], 2u, SIGGEN_N_MAX, &n) &&
        arg_u32(argv[4], SIGGEN_PLAY_HZ_MIN, SIGGEN_PLAY_HZ_MAX, &hz)) {
        bool snap = false, force = false, oc = false;
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "snap") == 0)       { snap = true; }
            else if (strcmp(argv[i], "force") == 0) { force = true; }
            else if (strcmp(argv[i], "oc") == 0)    { oc = true; }
            else { usage(use); return; }
        }
        const siggen_result_t r = siggen_start((uint8_t)dac, n, hz, snap, force, oc ? 1u : 0u);
        if (r != SIGGEN_OK) { siggen_refused(r); return; }
        siggen_visit(siggen_print);
        return;
    }
    usage(use);
}
CMD_DEFINE(siggen, "siggen", cmd_siggen_fn, "siggen [set <p> <v> | on <dac> <n> <hz> [snap] [force] [oc] | off | regs]");

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
        usage("buf [samples per half 16..2048, even]  (stop first; the next start uses the new size)");
        return;
    }
    if (!capture_set_half_len(n)) {
        put_line("buf: stop the stream first, and give an even number");
        cmd_parser_fail();
        return;
    }
    put_kv("samples per half", capture_half_len());
}
CMD_DEFINE(buf, "buf", cmd_buf_fn, "buf [n] - samples per buffer half (16..2048, even)");

static void cmd_dac_fn(int argc, char **argv)
{
    uint32_t unit, low = 0x100u, high = 0xF00u, slp = 8u;
    /* A trailing "force" skips the datasheet's limits (dac_triangle_force()) -
     * the defaults above stay the checked ones, "force" is the deliberate
     * override. */
    bool force = false;
    if ((argc >= 4) && (strcmp(argv[argc - 1], "force") == 0)) {
        force = true;
        argc--;
    }
    if ((argc < 3) || !arg_u32(argv[1], 1u, DAC_UNITS, &unit)) {
        usage("dac <1|2> <on|off> [low] [high] [slpdat]  (triangle on DACOUT1 = RA1 or DACOUT2 = RA8)");
        return;
    }
    siggen_release_dac((uint8_t)unit);     /* the generator yields its DAC (SG.3) */
    if ((argv[2][0] == 'o') && (argv[2][1] == 'f')) {
        dac_off((uint8_t)unit);
        put_kv("dac", unit);
        put_line("off");
        return;
    }
    if ((argv[2][0] != 'o') || (argv[2][1] != 'n') || (argc > 6) ||
        ((argc >= 4) && !arg_u32(argv[3], 0u, 4095u, &low)) ||
        ((argc >= 5) && !arg_u32(argv[4], 0u, 4095u, &high)) ||
        ((argc == 6) && !arg_u32(argv[5], force ? 0u : 1u, force ? 65535u : 255u, &slp)) ||
        (!force && (high <= low))) {
        usage("dac <1|2> <on|off> [low] [high] [slpdat] [force]  (0..4095, high > low, slpdat 1..255; "
              "force: any low/high 0..4095, slpdat 0..65535)");
        return;
    }
    if (!force && !dac_triangle_limits_ok((uint16_t)low, (uint16_t)high, (uint16_t)slp)) {
        /* Name the limit that was broken, with its number (slp <= 255 here,
         * so neither bound under- or overflows): the old single line blamed
         * SLPDAT even when low was the one out of range. */
        char num[16];
        if (low < DAC_CODE_MIN + slp) {
            (void)u32_to_str(num, DAC_CODE_MIN + slp);
            cmd_parser_write("dac: refused - low must be >= 0xCD + slpdat = ");
        } else {
            (void)u32_to_str(num, DAC_CODE_MAX - slp);
            cmd_parser_write("dac: refused - high must be <= 0xF32 - slpdat = ");
        }
        cmd_parser_write(num);
        put_line(" (p1422, Example 18-3 note 1); append 'force' to write it anyway");
        cmd_parser_fail();
        return;
    }
    if (!(force ? dac_triangle_force((uint8_t)unit, (uint16_t)low, (uint16_t)high, (uint16_t)slp)
                : dac_triangle_start((uint8_t)unit, (uint16_t)low, (uint16_t)high, (uint16_t)slp))) {
        put_line("dac: refused - CLKGEN7 did not come up");
        cmd_parser_fail();
        return;
    }
    if (force) {
        put_line(dac_triangle_limits_ok((uint16_t)low, (uint16_t)high, (uint16_t)slp)
                 ? "forced (within the datasheet's limits anyway)"
                 : "forced - OUTSIDE the datasheet's limits (p1422)");
    }
    put_kv("dac", unit);
    put_line(dac_pin_name((uint8_t)unit));
    put_kv("low", low);
    put_kv("high", high);
    put_kv("slpdat", slp);
    put_kv("period ns", dac_period_ns((uint8_t)unit));
}
CMD_DEFINE(dac, "dac", cmd_dac_fn, "dac <1|2> <on|off> [low] [high] [slpdat] [force] - triangle on DACOUT1/2");

/* The DAC test measures the DAC inside the chip: UREFCON puts DAC2 on
 * the UREF line and the ADC samples it as AN7, which every core has
 * (board.h). So no core is switched and no pin is involved - run 10 ran
 * the test on core 3 against RA8, which belongs to core 5, and measured
 * an open pin. The input in use is restored afterwards, so a "dactest"
 * from the console does not silently leave the measurement elsewhere. */
/* Not static: src/tests/bench.c's test_dac()/cmd_matrix() call this too
 * (declared extern there) instead of routing DAC2/UREF a second time. */
uint32_t run_dactest(uint32_t bursts)
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
    const uint32_t rc = dactest_run(bursts);
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

/* min/max/mean/pp of the completed half - shared by "stats", "stream"
 * and the periodic [half] line. The arithmetic is lib/stats.c since P2.2
 * (27.09.2026); this wrapper only picks the half.
 *
 * The cast drops `volatile`: capture_completed_half() is the half the DMA
 * finished last and is not writing - the DMA is filling the OTHER half -
 * so the samples do not change under the reader and the compiler may read
 * them as ordinary memory. (Should the reader be slower than one half
 * period, the DMA wraps into this half; that race existed with volatile
 * too - each sample is read once either way - and is what `late` counts.) */
static void completed_half_stats(uint32_t *mn, uint32_t *mx, uint32_t *mean)
{
    half_stats((const uint16_t *)capture_completed_half(), capture_half_len(),
               mn, mx, mean);
}

void console_half_stats(void)
{
    char line[96];
    uint32_t mn, mx, mean;
    completed_half_stats(&mn, &mx, &mean);
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
    completed_half_stats(&mn, &mx, &mean);
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
        usage("dump [count 1..4096] [offset 0..4095]  (the whole buffer; use snap first)");
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

/* The chain test (chaintest.c). "chain all" is the one command the
 * person at the board types; the rest is for repeating a part. */
static void cmd_chain_fn(int argc, char **argv)
{
    static const char use[] = "chain all | chain <0..9> | chain from <0..9> | chain run <ksps> [seconds]";
    uint32_t a = 0u, b = 0u;
    if ((argc == 2) && (strcmp(argv[1], "all") == 0)) {
        chain_all(0u, 9u);
    } else if ((argc == 3) && (strcmp(argv[1], "from") == 0) && arg_u32(argv[2], 0u, 9u, &a)) {
        chain_all(a, 9u);
    } else if ((argc == 2) && arg_u32(argv[1], 0u, 9u, &a)) {
        chain_all(a, a);
    } else if (((argc == 3) || (argc == 4)) && (strcmp(argv[1], "run") == 0) &&
               arg_u32(argv[2], 1u, 40000u, &a) &&
               ((argc == 3) || arg_u32(argv[3], 1u, 3600u, &b))) {
        chain_run(a, (argc == 4) ? b : 10u);
    } else {
        usage(use);
    }
}
CMD_DEFINE(chain, "chain", cmd_chain_fn, "chain all|<n>|from <n>|run <ksps> [s] - the chain test");

/* The chain as the example runs it (chaintest.c): "stream on <ksps>"
 * starts it and returns - main() processes every half from then on -
 * "stream" reports, "stream off" stops, "stream grab" halts it just long
 * enough to send one contiguous window to the GUI and restarts it (own
 * binary frame, gui_link_stream_grab(), src/link/gui_link.c - moved out
 * of this file in P6.4, 27.09.2026). The report is a snapshot
 * taken first and printed afterwards; printing takes the CPU from main()
 * for a few milliseconds, so at high rates each "stream" costs some
 * halves, which then show in the NEXT report's missed count. */
static void cmd_stream_fn(int argc, char **argv)
{
    static const char use[] = "stream on <ksps 1..40000> [<core 1..5> <pinsel 0..15> [<samc 0..31>]] | stream off | stream grab | stream";
    uint32_t k = 0u, core = 0u, pin = 0u, samc = 0u;
    /* "stream on <ksps>": core 5, RA8, with the DAC2 triangle as the test
     * signal. "stream on <ksps> <core> <pinsel> [<samc>]": that input,
     * the DAC left to the "dac" command. */
    if ((argc >= 3) && (argc <= 6) && (argc != 4) && (strcmp(argv[1], "on") == 0) &&
        arg_u32(argv[2], 1u, 40000u, &k) &&
        ((argc == 3) || (arg_u32(argv[3], 1u, 5u, &core) && arg_u32(argv[4], 0u, 15u, &pin) &&
                         ((argc == 5) || arg_u32(argv[5], 0u, 31u, &samc))))) {
        if ((argc == 3) && (siggen_dac() == 2u)) {
            /* the test form puts its triangle on DAC2 (routing refuses it
             * with ROUTE_ERR_DAC_BUSY) - say why instead of "set-up failed" */
            put_line("stream: DAC2 plays the signal generator - 'siggen off', or 'stream on <ksps> 5 3' to read it");
            cmd_parser_fail();
            return;
        }
        const bool ok = (argc == 3) ? chain_stream_on(k)
                                    : chain_stream_on_input(k, (uint8_t)core, (uint8_t)pin, (uint8_t)samc, false);
        if (!ok) { put_line("stream: set-up failed (clock, core or trigger) - run 'chain 0'"); cmd_parser_fail(); return; }
    } else if ((argc == 2) && (strcmp(argv[1], "off") == 0)) {
        chain_stream_off();
        put_line("stream: off, boot configuration restored");
        return;
    } else if ((argc == 2) && (strcmp(argv[1], "grab") == 0)) {
        gui_link_stream_grab();
        return;
    } else if (argc != 1) {
        usage(use);
        return;
    }
    uint32_t ksps = 0u, free_cyc = 0u;
    uint64_t xfer = 0u;
    if (!chain_streaming()) { put_line("stream: off"); return; }
    const bool running = chain_stream_state(&ksps, &xfer, &free_cyc);
    /* snapshot, then print */
    const uint32_t halves = blocks_done, ov = dma_overrun, la = late_service, mi = proc_missed;
    const uint32_t pmax = proc_ticks_max;
    const bool brake = capture_overrun_aborted();
    uint32_t mn = 0u, mx = 0u, mean = 0u;
    completed_half_stats(&mn, &mx, &mean);
    put_line(running ? "stream: on - SCCP1 -> ADC (single conversion) -> DMA0 -> ping-pong -> main()"
                     : "stream: STOPPED by itself - the overrun brake fired, the rate is not usable");
    put_kv("ksps", ksps);
    put_kv("core", adc_core());
    put_kv("pinsel", adc_pinsel());
    put_kv("seconds", (ksps != 0u) ? (uint32_t)(xfer / ((uint64_t)ksps * 1000u)) : 0u);
    put_kv("transfers (millions)", (uint32_t)(xfer / 1000000u));
    put_kv("halves", halves);
    put_kv("overrun", ov);
    put_kv("late", la);
    put_kv("missed", mi);
    put_kv("brake", brake ? 1u : 0u);
    put_kv("processing max per half, us", (pmax * 8u + 50u) / 100u);   /* 80 ns ticks */
    put_kv("free CPU cycles per sample (mean)", free_cyc);
    put_kv("last half min", mn);
    put_kv("last half max", mx);
    put_kv("last half mean", mean);
}
CMD_DEFINE(stream, "stream", cmd_stream_fn, "stream on <ksps> [core pinsel [samc]]|off|grab - the chain streaming, main() processing, one halt/transfer/restart cycle");

static void cmd_reset_fn(int argc, char **argv)
{
    (void)argc; (void)argv;
    put_line("resetting");
    console_flush();                    /* let the reply leave first */
    __asm__ volatile ("reset");
}
CMD_DEFINE(reset, "reset", cmd_reset_fn, "reset - software reset");

/* ------------------------------------------------------------------ */

/* P6.1: per-module registration, so cli_init() reads as a list of who owns
 * what instead of one flat block. Each function registers exactly the
 * commands its module owns, in their old relative order; cli_init() calls
 * the functions in the position their first command used to have, so the
 * overall registration order - and therefore "help"'s order - is unchanged.
 *
 * P6.2 moved the "sweep" and "test" command bodies (and everything they
 * drive: test_*, matrix_*, sweep_*) into src/tests/bench.c, so their
 * cmd_t objects are static there now and cli.c can no longer name them
 * directly - bench_register_sweep()/bench_register_test() (bench.h) do
 * that instead. They are two functions, not one bench_register(), because
 * "sweep" and "test" were never adjacent in the old order: "clk" and
 * "pll" (still owned by cli.c) sit between them, and cli_init() below
 * still calls cmd_register() in exactly the old sequence. */

/* The binary block transfer (docs/PLAN-BINARY-TRANSFER.md): snap/rate/blk.
 * link_register() itself moved to src/link/gui_link.c in P6.4 (27.09.2026),
 * along with the commands it registers - called here exactly as before,
 * now an extern from gui_link.h instead of a function defined in this
 * file. */

/* The triggered chain test and its streaming/GUI grab cycle. */
static void chain_register(void)
{
    (void)cmd_register(&cmd_chain);
    (void)cmd_register(&cmd_stream);
}

void cli_init(void)
{
    /* The clocks have changed under the baud generator: re-set it for
     * the 100 MHz peripheral clock, after the last FRC-timed byte is out.
     * Only when the divider really changes: in the simulator build the
     * CPU never leaves the FRC, and toggling ON while the simulator's
     * UART model is still transmitting leaves its transmitter dead
     * (TXWRE set, nothing gets out any more) - uart_set_baud() makes
     * that check and reports back whether it did anything. */
    if (uart_set_baud(UART_BRG_PLL)) {
        console_trace("[boot] uart reclocked to PLL2, 115200 8N1\r\n");
    }

    cmd_parser_init(console_write);
    cmd_parser_set_yield(console_yield);     /* after init - init clears it */
    (void)cmd_register(&cmd_version);
    (void)cmd_register(&cmd_status);
    (void)cmd_register(&cmd_regs);
    (void)cmd_register(&cmd_route);
    (void)cmd_register(&cmd_siggen);
    (void)cmd_register(&cmd_start);
    (void)cmd_register(&cmd_stop);
    (void)cmd_register(&cmd_samc);
    (void)cmd_register(&cmd_input);
    (void)cmd_register(&cmd_selftest);
    (void)cmd_register(&cmd_stats);
    (void)cmd_register(&cmd_dump);
    (void)cmd_register(&cmd_clear);
    (void)cmd_register(&cmd_led);
    bench_register_sweep();
    (void)cmd_register(&cmd_clk);
    (void)cmd_register(&cmd_pll);
    bench_register_test();
    (void)cmd_register(&cmd_core);
    (void)cmd_register(&cmd_buf);
    (void)cmd_register(&cmd_dac);
    (void)cmd_register(&cmd_dactest);
    link_register();
    chain_register();
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

    /* Receive interrupt: enabled last, so that nothing typed early runs a
     * command before the measurement is set up (uart_enable_rx_irq()). */
    uart_enable_rx_irq(UART_RX_PRIORITY);

    cmd_parser_prompt();                     /* sync point for a reader */
}

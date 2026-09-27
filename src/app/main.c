/*
 * main.c
 *
 * Start-up sequence and main loop of the ADC/DMA example on the EV74H48A
 * (dsPIC33 Curiosity Platform Development Board, dsPIC33AK512MPS512 DIM).
 *
 * THE FIRMWARE RUNS NO TEST BY ITSELF. It boots, brings the console up
 * and waits. That is deliberate: through seven board runs the console
 * never received a byte (`rx = 0`), and it could not be told whether the
 * bytes never reached the pin or whether the receive interrupt was
 * starved behind the DMA interrupt, which fires 1.6 million times a
 * second at full rate. With nothing converting after the boot, that
 * question answers itself - anything typed either echoes or it does not.
 * Everything else then runs on command: "test" lists the parts, "test
 * all" runs them in order (see cli.c).
 *
 * The order below matters and is the whole story of this file:
 *
 *   1. LED0 first, so that a stop code can be shown from the very first
 *      checkpoint on.
 *   2. The UART, still on the 8 MHz FRC, so that every following step
 *      reports itself on the terminal and a failure inside clock_init()
 *      is readable, not just a blink code.
 *   3. Clocks: FRC -> PLL1 320 MHz for the ADC, PLL2 200 MHz for the CPU.
 *      Stops with blink code 1..4 if a step does not complete.
 *   4. The console proper: baud generator re-set for the 100 MHz
 *      peripheral clock, parser, banner, receive interrupt. It runs in
 *      the UART receive interrupt from here on.
 *   5. ADC core and DMA channel, configured and idle, and the ADC clock
 *      set to the slowest ratio the part allows (ADC_CLKDIV in board.h,
 *      /10 = 32 MHz = 4 MSPS). Nothing converts: no burst is triggered,
 *      so no DMA event and no interrupt can come from the ADC side.
 *   6. The main loop serves the console and, while a test runs the
 *      stream, processes each completed buffer half.
 *
 * Everything hardware-specific lives in the modules - board.h (pins,
 * ADC core), clock.c, adc.c, dma.c (sim_dma.c in the simulator build),
 * capture.c (counters, burst restart, self-test), led.c,
 * diag.c (stop codes, traps) and cli.c (UART, commands); this file only
 * sequences them.
 */

#include <xc.h>
#include "board.h"
#include "clock.h"
#include "adc.h"
#include "dma.h"
#include "capture.h"
#include "led.h"
#include "diag.h"
#include "timebase.h"
#include "console.h"
#include "crc16.h"
#include "sim.h"

/* Status line every ~5 s for the first minute, then every ~60 s, while a
 * test has the stream running. 39 062 halves per second at 40 MSPS. In
 * the simulator a half costs a few thousand instructions of the stand-in,
 * so count halves, not time. */
#ifdef __MPLAB_DEBUGGER_SIMULATOR
#define STATUS_EVERY_HALVES   50u
#else
#define STATUS_EVERY_HALVES   195312u
#endif
#define STATUS_FAST_LINES     12u

#if defined(__MPLAB_DEBUGGER_SIMULATOR) && defined(SIM_SMOKE)
/* The smoke build (tools\build.bat smoke, tools\sim_trap.py --smoke):
 * the simulator boots this firmware exactly as the normal simulator
 * build does, but instead of the ping-pong stream it types a fixed
 * script into the console and stops. What it proves is that the boot
 * path and the console reach the far side without a CPU trap - stack,
 * alignment, RAM layout - in under a minute, after every change that
 * touches them (docs/IMPLEMENTATION-PLAN.md, rule 6). Nothing here
 * exists in any other build.
 *
 * The script goes in through cmd_parser_feed_char(), the same entry the
 * receive interrupt uses on silicon, one byte at a time, CR-terminated:
 * the simulator's UART receiver takes no injected bytes (MDB's uart2io
 * input options are silently ignored on this device), and the receive
 * interrupt would not be dispatched anyway. Echo, dispatch, reply and
 * prompt all run as they would on the board, only in main() context.
 *
 * tests/smoke/expected.log is what this printed when it was written;
 * sim_trap.py --smoke diffs the run against it with the build-id lines
 * masked. A command added to the script, or a changed reply, is a
 * change to expected.log in the same commit. */
#include "cmd_parser.h"

static const char *const smoke_script[] = {
    "help",       /* every registered command - the table is full when one is missing */
    "version",    /* [build] block, board, input, console                              */
    "status",     /* the counters, all idle                                            */
    "route list", /* P11.5: no route active in the simulator - nothing streams here    */
};

static void smoke_run(void)
{
    for (size_t i = 0; i < sizeof smoke_script / sizeof smoke_script[0]; i++) {
        const char *s = smoke_script[i];
        console_puts("[smoke] > ");
        console_puts(s);
        console_puts("\r\n");
        while (*s != '\0') { cmd_parser_feed_char(*s++); }
        cmd_parser_feed_char('\r');      /* Enter: the line is dispatched here */
    }
#ifdef SIM_SMOKE_FAULT
    /* The negative test (build.bat smoke fault [n]): a deliberate CPU
     * trap after the script, which sim_trap.py --smoke must report as a
     * failure - no "[smoke] DONE", a [TRAP] block if the handler runs,
     * INTCON1's trap flags set after the halt. Numbered, because the
     * simulator does not model every trap the silicon raises (which one
     * it does is recorded in tests/baseline.md):
     *   1  a 32-bit read from an odd address (the plan's case). The
     *      address goes through a volatile variable: given a constant
     *      odd address, xc-dsc (-O1) quietly emits four byte loads and
     *      no misaligned access ever happens (seen 27.09.2026). With
     *      the address opaque it emits the mov.l.
     *   2  a 32-bit read from an address outside every memory region
     *      (0x00FF0000: above data RAM, below the SFR/flash windows).
     *   3  a stack overflow: W15 pushed past SPLIM.
     *   4  an illegal opcode: a jump into a data table of zeros is not
     *      possible in flash, so 0xFFFFFFFF placed in flash and called.
     * Every one of them is a bug the smoke run exists to catch, so the
     * simulator's answer to each is worth knowing. Measured 27.09.2026
     * with MPLAB X v6.35: 1 and 2 execute without any trap (the read
     * returns 0, "[smoke] DONE" follows, only the expected.log diff
     * fails the run); 3 sets INTCON1.STKERR and the simulator aborts
     * with E0110 at the trap dispatch; 4 aborts with W0014 "Invalid
     * opcode" and E0110. build.bat therefore defaults to 3. */
    {
        static volatile uint8_t   fault_bytes[8] __attribute__((aligned(4)));
        static volatile uintptr_t fault_addr;
        console_kv("[smoke] FAULT case", SIM_SMOKE_FAULT);
#if SIM_SMOKE_FAULT == 1
        fault_addr = (uintptr_t)fault_bytes + 1u;
        console_puts("[smoke] FAULT: misaligned 32-bit read follows\r\n");
        console_kv("[smoke] read", *(volatile uint32_t *)fault_addr);
#elif SIM_SMOKE_FAULT == 2
        fault_addr = 0x00FF0000u;
        (void)fault_bytes;
        console_puts("[smoke] FAULT: 32-bit read from an unmapped address follows\r\n");
        console_kv("[smoke] read", *(volatile uint32_t *)fault_addr);
#elif SIM_SMOKE_FAULT == 3
        (void)fault_bytes; (void)fault_addr;
        console_puts("[smoke] FAULT: stack pushed past SPLIM follows\r\n");
        __asm__ volatile ("mov.l SPLIM, w0\n\t"
                          "add.l w0, #64, w0\n\t"
                          "mov.l w0, w15\n\t"
                          "mov.l w0, [w15++]" ::: "w0", "memory");
        console_puts("[smoke] FAULT: still running after the push\r\n");
#elif SIM_SMOKE_FAULT == 4
        (void)fault_bytes; (void)fault_addr;
        static const uint32_t bad_op[2] __attribute__((section(".text"), aligned(4))) = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        console_puts("[smoke] FAULT: call into an illegal opcode follows\r\n");
        ((void (*)(void))(uintptr_t)bad_op)();
        console_puts("[smoke] FAULT: still running after the call\r\n");
#else
#error "SIM_SMOKE_FAULT must be 1..4"
#endif
    }
#endif
}
#endif /* smoke build */

int main(void)
{
    /* Persistent RAM is undefined on the very first power-up (no start-up
     * code clears it, which is the point). A value outside the known
     * range means "no usable history", so normalise it before anything
     * reads it. */
    if (boot_stage > 9u) { boot_stage = 0u; trap_seen = 0u; trap_vec = 0u; trap_stage = 0u; }

    /* BR.6 (27.09.2026): paint the free stack before the first call that
     * could go deeper than this one - see diag.c for how and why. */
    diag_stack_paint();

    led_init();
    boot_mark(1u);
    console_early_init();
    boot_mark(2u);
    /* A banner nobody can miss: where the log of one run begins. */
    console_puts("\r\n\r\n"
                 "##############################################################\r\n"
                 "##   ADC/DMA TEST LOG  -  START OF RUN  (copy from here)    ##\r\n"
                 "##############################################################\r\n"
                 "\r\n");
    console_puts("[boot] " BUILD_ID "\r\n");
    SIM_BANNER();                     /* simulator build: say so first   */
    diag_report_reset();              /* why are we booting? RCON        */

    /* Did the previous run end in a trap? boot_stage/trap_* live in
     * persistent RAM, so say so now - an unhandled trap ends in "reset"
     * when no debugger is attached, and without this the board would just
     * appear to restart for no reason. */
    if (trap_seen != 0u) {
        console_puts("[boot] WARNING the previous run ended in a trap\r\n");
        console_kv("[boot] trap count", trap_seen);
        console_kv("[boot] last trap vector", trap_vec);
        console_kv("[boot] boot stage when it hit", trap_stage);
        console_puts("[boot] see [TRAP] in the earlier log, or docs/TROUBLESHOOTING.md 2.0b\r\n");
        trap_seen = 0u;          /* reported once; the next trap re-arms it */
    }

    /* A chain run that never reached its @END: say in which stage it
     * was, so that the log of the next boot carries it. */
    if ((chain_mark & 0xFFFF0000u) == CHAIN_MARK_MAGIC) {
        console_kv("[boot] WARNING the last 'chain' run ended without @END, in stage S", chain_mark & 0xFFu);
        console_puts("[boot] 'chain from <stage>' continues after it\r\n");
    }
    chain_mark = 0u;

    boot_mark(3u);
    clock_init();
    boot_mark(4u);
    cli_init();
    boot_mark(5u);

    /* Timer1 as the stopwatch. It used to be started by timebase_check(),
     * which only the sweep calls - so a run that only did "test dac"
     * measured its window as zero ticks (run 12). Nothing else times
     * itself off it, so starting it here costs nothing and means every
     * measurement has a clock. */
    timebase_init();
    /* One line, because the block transfer stands or falls with both ends
     * agreeing on the CRC variant, and "CRC-16 CCITT" names at least four
     * of them. The check value of "123456789" is what pins it down; the
     * Python side asserts the same constant. */
    console_puts(crc16_selfcheck()
                 ? "[boot] crc16/ccitt-false self-check: ok (0x29B1)\r\n"
                 : "[boot] crc16/ccitt-false self-check: FAILED\r\n");
    adc_init(ADC_PINSEL, ADC_SAMC, SAMPLES_PER_BUF_MAX);
    boot_mark(6u);
    capture_init();
    boot_mark(7u);

    /* Start at the slowest rate the ADC may run, not at the fastest. It
     * is the setting the DMA should manage comfortably, so the first test
     * anyone runs is the one most likely to pass - and a failure there is
     * the chain itself, not the rate. "clk" changes it. */
    {
        const uint32_t rc = capture_set_pll(board_cfg.adc_pll_postdiv1, board_cfg.adc_pll_postdiv2);
        console_kv("[boot] pll1 postdiv1", clock_adc_pll_postdiv1());
        console_kv("[boot] pll1 postdiv2", clock_adc_pll_postdiv2());
        console_kv("[boot] adc clock Hz", clock_adc_hz());
        console_kv("[boot] sample rate ksps (back-to-back)", capture_nominal_ksps(0u));
        if (rc != CLKDIV_OK) {
            console_puts("[boot] WARNING the ADC clock did not switch: ");
            console_puts(clock_adc_div_error(rc));
            console_puts("\r\n");
        }
    }
    boot_mark(8u);

#if defined(__MPLAB_DEBUGGER_SIMULATOR) && defined(SIM_SMOKE)
    /* Smoke build: the script instead of the stream, then the marker
     * line the runner waits for. No stream is started, so the main loop
     * below only idles (the stand-in produces nothing while no burst
     * runs) and the marker stays the last line of the log. */
    console_puts("[boot] simulator: smoke build, running the command script\r\n");
    smoke_run();
    console_puts("[smoke] DONE\r\n");
#elif defined(__MPLAB_DEBUGGER_SIMULATOR)
    /* The simulator's job is the ping-pong check, which needs the stream
     * and has nobody to type "test". SIM_HALF_LEN (build.bat sim <n>)
     * runs it at another buffer size, to show the run-time length reaches
     * ADC burst, DMA block and the consumers. */
#ifdef SIM_HALF_LEN
    console_kv("[boot] simulator: samples per half set to", SIM_HALF_LEN);
    (void)capture_set_half_len(SIM_HALF_LEN);
#endif
    console_puts("[boot] simulator: starting the stream for the ping-pong check\r\n");
    counters_clear();
    capture_start();
#else
    /* Idle, and saying so: nothing converts until a test is typed. If a
     * character typed now does not echo, the console never receives - and
     * that is a finding, not a side note. */
    console_puts("\r\n"
                 "[boot] READY - nothing is converting, the console has the CPU\r\n"
                 "[boot] type 'help' for all commands, 'test' for the parts of a run,\r\n"
                 "[boot] 'test all' for the whole thing. Please log this terminal from\r\n"
                 "[boot] power-up and send it back.\r\n\r\n");
#endif
    boot_mark(9u);
    led_mode(2u);                     /* heartbeat                       */

    uint32_t idle = 0;
    uint32_t next_status = STATUS_EVERY_HALVES;
    uint32_t status_lines = 0;

    for (;;) {
        SIM_DMA_TICK();               /* simulator: one half per pass    */
        if (capture_service()) {
            idle = 0;
            /* No status lines while "stream on" runs: printing takes the
             * CPU from this loop for milliseconds and would itself cause
             * missed halves. "stream" reports on request. */
            if ((blocks_done >= next_status) && !capture_chain_active()) {
                if (SIM_CHECK_RUNNING()) {
                    /* Simulator: the UART is slow, keep it quiet while
                     * the ping-pong check runs. Empty on silicon. */
                    next_status = blocks_done + STATUS_EVERY_HALVES;
                } else {
                    console_status_line();
                    console_half_stats();     /* is there a signal?  */
                    status_lines++;
                    next_status += (status_lines < STATUS_FAST_LINES)
                                   ? STATUS_EVERY_HALVES
                                   : 12u * STATUS_EVERY_HALVES;
                }
            }
        } else if (capture_running() && !capture_chain_active()) {
            /* Burst mode only: a triggered stream at 1 kSPS delivers one
             * half per second, far longer than this loop's idle bound. */
            /* The stream stopped: burst restart lost, or the DMA shut
             * itself off. Say so instead of sitting here silently. */
            if (!dma0_enabled())     { fail(8u); }
            if (++idle > WAIT_LIMIT) { fail(6u); }
        } else {
            idle = 0;                 /* idle: the console has the CPU   */
        }

        /* What to look at with the debugger, the "status" command or the
         * [stat] lines:
         *   blocks_done   x half_len / elapsed time = actual rate
         *                 (includes the re-trigger gap once per buffer)
         *   dma_overrun   must stay 0, otherwise the DMA bus lost samples
         *   late_service  must stay 0, otherwise the ISR is too slow
         *   proc_missed   must stay 0, otherwise main() is too slow
         *   last_sample   changing means data is really moving
         *   selftest_mean ~3840 = the chain was proven before AN5 was used
         *   fail_code     0 while running; the LED pattern otherwise
         */
    }

    return 0;
}

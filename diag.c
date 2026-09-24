/*
 * diag.c
 *
 * Diagnostics of the ADC/DMA example: the stop codes (fail()), the trap
 * and unhandled-interrupt handler, the boot-stage record that survives a
 * reset, and the register dump. This is the one module that may look at
 * every other module's registers - it reports, it does not configure.
 */

#include <xc.h>
#include <stdbool.h>
#include <libpic30.h>       /* __delay32()                                 */
#include "diag.h"
#include "clock.h"
#include "adc.h"
#include "dma.h"
#include "capture.h"
#include "dac.h"
#include "led.h"
#include "console.h"

volatile uint32_t fail_code = 0;       /* != 0: stopped, see fail()        */

/* How far start-up got. Written at every step (boot_mark()) and printed
 * by the trap handler, so that a trap which happens *before* the console
 * exists - or one that reboots the part before anything drains - can
 * still be located afterwards. Deliberately not initialised: it lives in
 * the no-init section so a reset does not wipe it, which is what makes
 * the "it just reboots in a loop" case readable. See trap_report(). */
volatile uint32_t boot_stage __attribute__((persistent));
volatile uint32_t trap_seen  __attribute__((persistent));
volatile uint32_t trap_vec   __attribute__((persistent));
volatile uint32_t trap_stage __attribute__((persistent));

/* ------------------------------------------------------------------ *
 * Stop here and say why - with the LED, because at this point there
 * may be no debugger attached and no clock to speak of.
 *
 *   code  meaning                                   where
 *   1     PLL1 (ADC clock) did not configure/lock   clock_init()
 *   2     PLL2 (system clock) did not configure/lock clock_init()
 *   3     CLKGEN1 did not switch to PLL2             clock_init()
 *   4     CLKGEN6 did not switch to PLL1             clock_init()
 *   5     ADC core never became ready (ADRDY)        adc_init()
 *   6     no DMA blocks arrived (nothing moves)      self-test / run
 *   7     self-test value out of range               self-test
 *   8     DMA channel switched itself off (CHEN = 0) self-test / run
 *   9     CPU trap or unhandled interrupt            _DefaultInterrupt()
 *   10    clock fail: FSCM moved the CPU to BFRC     _CLKFInterrupt()
 *   11    guard words behind the buffer changed      capture.c guard_check()
 *   12    (free - the rate test is part of the "test" command now)
 *
 * Pattern: <code> short blinks, one long pause, repeat. The blink speed
 * depends on which clock the CPU is on at the time; the count is what
 * counts.
 * ------------------------------------------------------------------ */
static const char *const fail_text[] = {
    "no error",
    "PLL1 (ADC clock) did not configure or lock",
    "PLL2 (system clock) did not configure or lock",
    "CLKGEN1 did not switch",
    "CLKGEN6 did not switch to PLL1",
    "ADC core never reported ready (ADRDY)",
    "no DMA blocks arrived, or the stream stopped",
    "self-test mean outside 3648..4032",
    "DMA channel switched itself off (CHEN = 0)",
    "CPU trap or unhandled interrupt - see the [TRAP] lines",
    "clock fail - the FSCM moved the CPU to the backup FRC, see the [CLKF] lines",
    "something wrote past the end of the sample buffer - see the [guard] lines",
    "sample rate does not follow the repeat-timer period - see the [ratetest] lines",
};
#define FAIL_TEXT_N  (sizeof fail_text / sizeof fail_text[0])

/* ------------------------------------------------------------------ *
 * Traps and unhandled interrupts
 *
 * Why this exists: the start-up code links a weak __DefaultInterrupt
 * into all 364 vector slots, and it is literally two instructions,
 * "break" followed by "reset". With a debugger attached the break halts
 * the core - MPLAB X drops into a break session on a line nobody set a
 * breakpoint on - and without one the part silently reboots. Either way
 * the reason is lost. Only two slots are ours (DMA0 = IRQ 77, U2RX =
 * IRQ 102), so every other event on this device lands there.
 *
 * __DefaultInterrupt is weak, so defining it here takes over all 362
 * remaining slots at once. What we can say about the cause:
 *
 *   INTTREG.VECNUM  the vector number that fired (bits 8:0, read-only).
 *                   Subtract nothing - this is the IRQ number, and
 *                   Table 4-x / the pack's ATDF names it. 0 = the
 *                   collapsed "COMMON" vector, 1 = CPU/FPU (this is
 *                   where the CPU traps arrive on dsPIC33A - there is no
 *                   separate address-error or stack-error slot as on
 *                   dsPIC33C).
 *   INTTREG.ILR     the priority level it came in at.
 *   INTCON1         ADDRERR (bit 3), STKERR (bit 4), BADOPERR (bit 2).
 *   INTCON3         bus-error traps: XRAMBET, YRAMBET, DMABET, CPUBET.
 *   INTCON4         maths: DIV0ERR, plus accumulator overflow bits.
 *   INTCON5         DMTE / WDTE - deadman timer and watchdog.
 *
 * The handler prints all of that, keeps a copy in persistent RAM (so it
 * survives the reset a second trap would cause) and then blinks code 9.
 * It must not return: the condition that caused a trap is still there.
 * ------------------------------------------------------------------ */
void boot_mark(uint32_t stage)
{
    boot_stage = stage;
}

/* RCON bit -> name, from the device header (p33AK512MPS512.h, RCONBITS).
 * Note what is NOT here: this device has no TRAPR or IOPUWR flag, so a
 * trap that ends in a reset does not announce itself in RCON - only
 * trap_seen in persistent RAM (below) can tell, and only if the reset
 * was not a POR. */
static const struct { uint32_t mask; const char *name; } rcon_bits[] = {
    { _RCON_POR_MASK,    "POR"    },   /* power-on                          */
    { _RCON_BOR_MASK,    "BOR"    },   /* brown-out: the supply dipped      */
    { _RCON_WDTO_MASK,   "WDTO"   },   /* watchdog time-out                 */
    { _RCON_SWR_MASK,    "SWR"    },   /* software reset (reset command)    */
    { _RCON_EXTR_MASK,   "EXTR"   },   /* MCLR pin                          */
    { _RCON_CM_MASK,     "CM"     },   /* configuration mismatch            */
    { _RCON_BUCKR_MASK,  "BUCKR"  },   /* internal buck regulator reset     */
    { _RCON_VREG2R_MASK, "VREG2R" },   /* internal regulator resets         */
    { _RCON_VREG3R_MASK, "VREG3R" },
    { _RCON_VREG4R_MASK, "VREG4R" },
    { _RCON_IDLE_MASK,   "IDLE"   },   /* woke from Idle (not a reset)      */
    { _RCON_SLEEP_MASK,  "SLEEP"  },   /* woke from Sleep (not a reset)     */
};

void diag_report_build(void)
{
    console_puts("[build] " BUILD_ID "\r\n");
    console_puts("[build] board: " BOARD_NAME "\r\n");
    console_kv("[build] adc core", ADC_INSTANCE);
    console_kv("[build] default input (pinsel)", ADC_PINSEL);
    console_kv("[build] default samc", ADC_SAMC);
    console_kv("[build] adc clock divider x100", ADC_CLKDIV);
    console_kv("[build] samples per half (max)", SAMPLES_PER_HALF_MAX);
    console_kv("[build] samples per half (in use)", capture_half_len());
    console_kv("[build] boot_verbose", BOOT_VERBOSE);
#ifdef __MPLAB_DEBUGGER_SIMULATOR
    console_puts("[build] simulator build (sim_dma.c, no ADC, no DMA)\r\n");
#endif
}

void diag_report_reset(void)
{
    const uint32_t rcon = RCON;
    console_kv_hex("[boot] RCON", rcon);
    console_puts("[boot] reset cause:");
    bool any = false;
    for (uint32_t i = 0; i < sizeof rcon_bits / sizeof rcon_bits[0]; i++) {
        if (rcon & rcon_bits[i].mask) {
            console_puts(" ");
            console_puts(rcon_bits[i].name);
            any = true;
        }
    }
    if (!any) {
        console_puts(" none flagged - a reset that RCON does not record, e.g. a"
                     " trap that could not be handled, or a debugger restart");
    }
    console_puts("\r\n");
    RCON = 0u;                        /* each boot reports its own cause  */
}

static const char *const boot_text[] = {
    "before main()",                     /* 0 - persistent RAM was clear */
    "led_init() done",
    "console_early_init() done",
    "clock_init() entered",
    "clock_init() done",
    "cli_init() done",
    "adc_init() done",
    "capture_init() done",
    "self-test done",
    "main loop running",
};

/* Print what is known about a trap. Blocking, and the console may not
 * exist yet - console_early_init() is stage 2, so anything below that
 * has no output and only the persistent copy plus the LED. */
static void trap_report(uint32_t vec)
{
    if (boot_stage >= 2u) {
        /* Not console_sync_baud(): that only corrects the baud divider and
         * trusts the console to be otherwise intact. A trap can have
         * disturbed the pins, the PPS mapping or the UART itself, so put
         * the whole path back up before relying on it. */
        console_force_up();
        console_puts("\r\n[TRAP] unhandled vector or CPU trap\r\n");
        console_kv("[TRAP] INTTREG.VECNUM", vec);
        console_kv("[TRAP] INTTREG.ILR", (uint32_t)INTTREGbits.ILR);
        /* PCTRAP holds the program counter at the moment of a trap. Look
         * it up in the .map file or with "xc-dsc-nm -n" on the ELF: the
         * function whose address is just below it is where it happened. */
        console_kv_hex("[TRAP] PCTRAP (PC at the trap)", PCTRAP);
        console_kv("[TRAP] reached boot stage", boot_stage);
        console_puts("[TRAP] last step completed: ");
        console_puts((boot_stage < 10u) ? boot_text[boot_stage] : "unknown");
        console_puts("\r\n");
        if (vec == 1u) {
            console_puts("[TRAP] vector 1 = CPU/FPU: read INTCON1/3/4 below\r\n");
        } else if (vec == 0u) {
            console_puts("[TRAP] vector 0 = COMMON (collapsed) interrupt\r\n");
        } else if ((vec >= 2u) && (vec <= 5u)) {
            console_puts("[TRAP] vector 2..5 = X RAM ECC / Y RAM ECC / PBU parity / NVM ECC:"
                         " a memory error. RAM on this device is ECC-protected and a word"
                         " never written since power-up has random check bits, so READING"
                         " uninitialised RAM raises this (persistent variables, a buffer"
                         " read before it was filled, a 16-bit write into a never-written"
                         " 32-bit word). PCTRAP above says where; INTCON3 below says"
                         " whether it escalated to a bus-error trap\r\n");
        } else if ((vec == 9u) || (vec == 10u)) {
            console_puts("[TRAP] vector 9/10 = clock fail / clock error: the FSCM saw"
                         " the system clock stop; OSCCTRL, PLL2CON, CLK1CON below\r\n");
        } else if ((vec >= adc_cur->ch0_irq) && (vec < adc_cur->ch0_irq + ADC_IRQ_COUNT)) {
            console_kv("[TRAP] a channel or comparator event of the ADC core in use"
                       " reached the CPU; it is meant to trigger only the DMA."
                       " The IECn word in the dump says whether it was enabled. ch0 irq", adc_cur->ch0_irq);
        } else {
            console_puts("[TRAP] a peripheral raised an interrupt we do not handle;"
                         " look up the number in the ATDF interrupt list\r\n");
        }
        console_kv_hex("[TRAP] INTCON1", INTCON1);
        console_kv_hex("[TRAP] INTCON3", INTCON3);
        console_kv_hex("[TRAP] INTCON4", INTCON4);
        console_kv_hex("[TRAP] INTCON5", INTCON5);
        /* The named bits, so nobody has to decode the words by hand. */
        console_kv("[TRAP] INTCON1.ADDRERR", (uint32_t)INTCON1bits.ADDRERR);
        console_kv("[TRAP] INTCON1.STKERR", (uint32_t)INTCON1bits.STKERR);
        console_kv("[TRAP] INTCON1.BADOPERR", (uint32_t)INTCON1bits.BADOPERR);
        console_kv("[TRAP] INTCON3.DMABET", (uint32_t)INTCON3bits.DMABET);
        console_kv("[TRAP] INTCON3.CPUBET", (uint32_t)INTCON3bits.CPUBET);
        console_kv("[TRAP] INTCON4.DIV0ERR", (uint32_t)INTCON4bits.DIV0ERR);
        console_kv("[TRAP] INTCON5.WDTE", (uint32_t)INTCON5bits.WDTE);
        console_kv("[TRAP] INTCON5.DMTE", (uint32_t)INTCON5bits.DMTE);
        console_kv("[TRAP] trap_seen count", trap_seen);
        regs_dump();
        console_puts("[TRAP] LED0 blinks 9 from now on; send this log back\r\n");
    }
}

void __attribute__((interrupt, no_auto_psv)) _DefaultInterrupt(void)
{
    const uint32_t vec = (uint32_t)INTTREGbits.VECNUM;

    /* Persistent first: if printing itself traps, the next boot can still
     * be told what happened. */
    trap_seen++;
    trap_vec   = vec;
    trap_stage = boot_stage;

    capture_halt();
    INTCON1bits.GIE = 0;

    /* LED on before the first character is attempted. Printing needs a
     * working UART and a sane clock; this needs neither, so a lit LED0
     * with nothing on the terminal is itself the message "trapped, and
     * the console did not survive it". */
    led_on();

    trap_report(vec);

    fail_code = 9u;
    const uint32_t ms100 = clock_cpu_hz() / 10u;
    for (;;) {
        for (uint32_t i = 0; i < 9u; i++) {
            led_on();  __delay32(2u * ms100);
            led_off(); __delay32(2u * ms100);
        }
        __delay32(10u * ms100);
    }
}

void fail(uint32_t code)
{
    fail_code = code;
    capture_halt();

    /* Say why, with everything a reader needs, before blinking forever.
     * The UART is up from the first line of main() on, so this works for
     * the clock steps too. */
    console_sync_baud();
    console_puts("\r\n");
    console_kv("[FAIL] code", code);
    console_puts("[FAIL] ");
    console_puts((code < FAIL_TEXT_N) ? fail_text[code] : "unknown code");
    console_puts("\r\n");
    regs_dump();
    console_puts("[FAIL] LED0 blinks the code from now on\r\n");

    /* 100 ms in CPU cycles: 200 MHz once PLL2 drives CLKGEN1, else the
     * 8 MHz FRC we started on - clock.c knows which. */
    const uint32_t ms100 = clock_cpu_hz() / 10u;
    for (;;) {
        for (uint32_t i = 0; i < code; i++) {
            led_on();  __delay32(2u * ms100);
            led_off(); __delay32(2u * ms100);
        }
        __delay32(10u * ms100);
    }
}

/* ------------------------------------------------------------------ *
 * Register dump - what Part 4 of docs/TROUBLESHOOTING.md asks for
 *
 * Each module prints its own registers; this only sets the order and
 * adds what belongs to nobody else.
 * ------------------------------------------------------------------ */
void regs_dump(void)
{
    clock_regs_dump();
    adc_regs_dump();
    dma0_regs_dump();
    capture_regs_dump();
    dac_regs_dump();
    console_regs_dump();
    console_puts("[regs] cpu\r\n");
    console_kv_hex("INTCON1", INTCON1);
    console_kv_hex("PCTRAP", PCTRAP);
    console_kv("fail_code", fail_code);
}

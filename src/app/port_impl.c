/*******************************************************************************
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * Subject to your compliance with these terms, you may use Microchip software
 * and any derivatives exclusively with Microchip products. It is your
 * responsibility to comply with third party license terms applicable to your
 * use of third party software (including open source software) that may
 * accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
 * EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
 * INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
 * WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
 * BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
 * FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 ******************************************************************************/

/*
 * port_impl.c - this project's implementation of the port layer
 * (src/port/log.h, src/port/panic.h; V2 of docs/REFACTORING-PROPOSAL.md,
 * P4.1 of docs/IMPLEMENTATION-PLAN.md, 27.09.2026)
 *
 * The drivers under src/drivers/ print and stop only through port_*; this
 * file is where the example connects them to its own console (cli.c) and
 * to fail() (diag.c). It lives in app/ because that choice - a UART
 * console, a blink code - is the application's, not the driver's.
 *
 * Trace harness (tests/trace): a scenario that links a driver using
 * port_* lists this file in its .sources as well, and the harness's
 * console_* and fail() stubs (tests/trace/harness/stubs.c) do the rest -
 * the text lands in the trace as `C` lines and a panic as an `F` line
 * exactly as when the driver called console_* and fail() directly. No
 * port stubs of its own, so there is one mapping, not two that could
 * drift apart.
 */

#include "log.h"
#include "panic.h"
#include "console.h"
#include "diag.h"
#include "capture.h"     /* capture_halt() for clock_fail_hook()          */
#include "clock.h"       /* clock_fail_hook()'s declaration                */

void port_log(const char *s)
{
    console_puts(s);
}

void port_log_kv(const char *key, uint32_t v, bool hex)
{
    if (hex) {
        console_kv_hex(key, v);
    } else {
        console_kv(key, v);
    }
}

/* The start-up trace: console_trace*() in cli.c print with BOOT_VERBOSE
 * 1 (board.h) and are empty functions with it 0, so the gate stays where
 * it always was and no driver carries it (P4.5). */
void port_trace(const char *s)
{
    console_trace(s);
}

void port_trace_kv(const char *key, uint32_t v, bool hex)
{
    if (hex) {
        console_trace_kv_hex(key, v);
    } else {
        console_trace_kv(key, v);
    }
}

void port_flush(void)
{
    console_flush();
}

void port_panic(uint32_t code)
{
    fail(code);
    /* fail() blinks the code forever (diag.c) but diag.h does not declare
     * it noreturn; without this loop the compiler reports a noreturn
     * function that returns. Never reached. */
    for (;;) { }
}

/* The clock driver's only upward call (clock.h, P4.7): what this
 * application does when the fail-safe clock monitor has moved the CPU
 * to the backup FRC, before _CLKFInterrupt() prints and stops - halt
 * the capture, then bring the console up again for the new clock, in
 * that order, exactly as the ISR did itself until P4.7 - and the boot
 * stage the report names. It lives here rather than in a file of its
 * own because this is already the file that binds the drivers to this
 * project's console and fail(); when P8 adds the DMA and ADC hooks, the
 * hooks can move to a file of their own together. In the trace harness
 * the two callees are stubs (or capture.c's real capture_halt() where
 * it is linked), so the fail golden sees the same two lines in the same
 * order. Overrides clock.c's weak default. */
uint32_t clock_fail_hook(void)
{
    capture_halt();
    console_force_up();
    return boot_stage;
}

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
 * log.h - the port layer's text output (V2 of docs/REFACTORING-PROPOSAL.md)
 *
 * A driver under src/drivers/ reports through these two functions and
 * through nothing else - no console.h, no diag.h, no capture.h. What
 * becomes of the text is the project's business, not the driver's: here
 * src/app/port_impl.c hands it to the console (console_puts(),
 * console_kv(), console_kv_hex() in cli.c); in a foreign project these
 * are two empty functions or a printf. In the register-trace harness
 * (tests/trace) the same port_impl.c is linked and the harness's console
 * stubs turn the text into `C` lines, so a golden trace sees exactly what
 * it saw when the driver called console_* itself.
 *
 * Nothing here formats: a line is sent as the caller wrote it, "\r\n"
 * included, and port_log_kv() reproduces console_kv()/console_kv_hex()'s
 * one-line "key: value" forms.
 *
 * The second pair, port_trace()/port_trace_kv(), is the start-up trace:
 * the same text, but whether it is printed at all is the project's
 * decision, not the driver's. Here that decision is BOOT_VERBOSE
 * (board.h), taken inside cli.c's console_trace*() - with it 0 (the
 * default) these lines cost a call and print nothing, exactly as the
 * drivers' former console_trace*() calls did. A driver therefore never
 * carries the gate itself, and the trace harness, whose console stubs
 * print both pairs alike, sees the same `C` lines as before (P4.5).
 */
#ifndef PORT_LOG_H
#define PORT_LOG_H

#include <stdint.h>
#include <stdbool.h>

/* One string, verbatim (the caller supplies its own "\r\n"). */
void port_log(const char *s);

/* "key: 123\r\n" with hex false, "key: 0x00000123\r\n" with hex true -
 * the console's console_kv() and console_kv_hex() forms. */
void port_log_kv(const char *key, uint32_t v, bool hex);

/* The same two forms for the start-up trace, printed only when the
 * project says so (console_trace()/console_trace_kv()/_kv_hex()). */
void port_trace(const char *s);
void port_trace_kv(const char *key, uint32_t v, bool hex);

/* Block until everything handed to port_log()/port_trace() so far has
 * left - for a driver about to change the clock the output runs on
 * (clock_init() before the CPU clock, and with it the baud rate,
 * changes 25x). console_flush() here; empty where output is not
 * buffered (P4.7). */
void port_flush(void);

#endif /* PORT_LOG_H */

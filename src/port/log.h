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

#endif /* PORT_LOG_H */

/*
 * fmt.h - small formatting helpers for the console (fmt.c)
 *
 * No printf, so the reply cost is predictable. Moved out of cli.c on
 * 27.09.2026 (P2.1) so that they can be tested on the host
 * (tests/host/test_fmt.c) and used by more than one module.
 *
 * All three write into `out`, terminate with '\0' and return a pointer to
 * that terminator, so calls chain: p = copy_str(p, " min="); p =
 * u32_to_str(p, mn); ... The caller sizes the buffer: u32_to_str writes at
 * most 10 digits + '\0' (11 bytes), u32_to_hex exactly "0x" + 8 hex
 * digits + '\0' (11 bytes), copy_str strlen(s) + 1 bytes. None of them
 * knows the buffer end.
 */
#ifndef FMT_H
#define FMT_H

#include <stdint.h>

/* Decimal, no padding, no sign: "0" .. "4294967295". */
char *u32_to_str(char *out, uint32_t v);

/* Fixed width: "0x" and eight upper-case hex digits, "0x00000000" ..
 * "0xFFFFFFFF". */
char *u32_to_hex(char *out, uint32_t v);

/* Copy s (with its terminator); no truncation, no length check. */
char *copy_str(char *out, const char *s);

#endif /* FMT_H */

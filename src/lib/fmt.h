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
#include <stdbool.h>

/* Decimal, no padding, no sign: "0" .. "4294967295". */
char *u32_to_str(char *out, uint32_t v);

/* Fixed width: "0x" and eight upper-case hex digits, "0x00000000" ..
 * "0xFFFFFFFF". */
char *u32_to_hex(char *out, uint32_t v);

/* Copy s (with its terminator); no truncation, no length check. */
char *copy_str(char *out, const char *s);

/* Parse a decimal "[-]digits[.digits]" (SG.4, 29.09.2026: the fractional
 * parameters of "siggen set" - f0, h2..h7, decay, amp) into millionths:
 * "0.3" -> 300000, "-1.5" -> -1500000, "10000" -> 10000000000. No strtof,
 * no locale, no exponent. At most FMT_DEC_FRAC_MAX (6) digits after the
 * point and FMT_DEC_INT_MAX (12) before it, at least one digit in all
 * (".5" and "5." are accepted); a lone "-" or ".", a second sign or
 * point, anything after the number: false, and *micro untouched. */
#define FMT_DEC_FRAC_MAX  6
#define FMT_DEC_INT_MAX   12
bool fmt_parse_dec(const char *s, int64_t *micro);

/* The reverse, for the status reply: millionths as "[-]int.ffffff" with
 * trailing zeros dropped and no point for a whole number ("0.3", "-1.5",
 * "10000"). At most 1 + 13 + 1 + 6 digits + '\0' = 22 bytes for any
 * value fmt_parse_dec() accepts; sized for int64's full range, 28 bytes. */
char *dec_to_str(char *out, int64_t micro);

#endif /* FMT_H */

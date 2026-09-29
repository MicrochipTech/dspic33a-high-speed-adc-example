/*
 * fmt.c - small formatting helpers for the console (see fmt.h)
 *
 * The three functions were `static` in cli.c until 27.09.2026 (P2.1) and
 * are moved here unchanged; only the `static` went, so that cli.c and the
 * host test tests/host/test_fmt.c link the same code.
 */

#include "fmt.h"

/* ------------------------------------------------------------------ *
 * Small formatting helpers - no printf, so the reply cost is predictable
 * ------------------------------------------------------------------ */
char *u32_to_str(char *out, uint32_t v)
{
    char tmp[11];
    int i = 0;
    do { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u);
    while (i > 0) { *out++ = tmp[--i]; }
    *out = '\0';
    return out;
}

char *u32_to_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    *out++ = '0'; *out++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        *out++ = digits[(v >> shift) & 0xFu];
    }
    *out = '\0';
    return out;
}

char *copy_str(char *out, const char *s)
{
    while (*s) { *out++ = *s++; }
    *out = '\0';
    return out;
}

/* ------------------------------------------------------------------ *
 * Decimal fractions in millionths (SG.4) - see fmt.h
 * ------------------------------------------------------------------ */
bool fmt_parse_dec(const char *s, int64_t *micro)
{
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    int64_t ip = 0;
    int     n_int = 0;
    while ((*s >= '0') && (*s <= '9')) {
        if (++n_int > FMT_DEC_INT_MAX) { return false; }
        ip = ip * 10 + (int64_t)(*s - '0');
        s++;
    }
    int64_t fp = 0;
    int     n_frac = 0;
    if (*s == '.') {
        s++;
        while ((*s >= '0') && (*s <= '9')) {
            if (++n_frac > FMT_DEC_FRAC_MAX) { return false; }
            fp = fp * 10 + (int64_t)(*s - '0');
            s++;
        }
    }
    if ((*s != '\0') || ((n_int + n_frac) == 0)) { return false; }
    for (int i = n_frac; i < FMT_DEC_FRAC_MAX; i++) { fp *= 10; }
    const int64_t v = ip * 1000000 + fp;
    *micro = neg ? -v : v;
    return true;
}

char *dec_to_str(char *out, int64_t micro)
{
    uint64_t m;
    if (micro < 0) { *out++ = '-'; m = (uint64_t)0 - (uint64_t)micro; }
    else           { m = (uint64_t)micro; }
    uint64_t ip = m / 1000000u;
    uint32_t fp = (uint32_t)(m % 1000000u);
    char tmp[21];
    int i = 0;
    do { tmp[i++] = (char)('0' + (ip % 10u)); ip /= 10u; } while (ip != 0u);
    while (i > 0) { *out++ = tmp[--i]; }
    if (fp != 0u) {
        *out++ = '.';
        char d[6];
        for (int k = 5; k >= 0; k--) { d[k] = (char)('0' + (fp % 10u)); fp /= 10u; }
        int last = 5;
        while (d[last] == '0') { last--; }
        for (int k = 0; k <= last; k++) { *out++ = d[k]; }
    }
    *out = '\0';
    return out;
}

/*
 * test_fmt.c - host-side test for lib/fmt.c (u32_to_str, u32_to_hex, copy_str)
 *
 * Built by tools\hosttest.bat with the installed MinGW gcc, against the
 * real src/lib/fmt.c - not a reimplementation.
 *
 * Every check writes into a buffer pre-filled with a sentinel byte (0xAA)
 * and afterwards verifies (a) the text, (b) the returned pointer is the
 * terminator, and (c) every byte behind the terminator still holds the
 * sentinel - i.e. the function wrote exactly strlen + 1 bytes and nothing
 * beyond. That is what the callers in cli.c rely on when they size
 * `char num[16]` from "at most 10 digits or 10 hex characters".
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fmt.h"
#include "check.h"

#define BUF 32
#define SENTINEL 0xAAu

static char buf[BUF];

static void fill(void)
{
    memset(buf, (int)SENTINEL, sizeof buf);
}

/* True if buf[from..BUF) is untouched sentinel. */
static int guard_ok(size_t from)
{
    for (size_t i = from; i < sizeof buf; i++) {
        if ((unsigned char)buf[i] != SENTINEL) { return 0; }
    }
    return 1;
}

/* One decimal case: text, length, returned pointer, guard. */
static void check_str(uint32_t v, const char *expect)
{
    fill();
    char *end = u32_to_str(buf, v);
    CHECK(strcmp(buf, expect) == 0);
    CHECK_EQ((unsigned long)(end - buf), strlen(expect));
    CHECK_EQ((unsigned char)*end, 0u);
    CHECK(guard_ok(strlen(expect) + 1u));
}

/* One hex case: the same, and the width is always 10 characters. */
static void check_hex(uint32_t v, const char *expect)
{
    fill();
    char *end = u32_to_hex(buf, v);
    CHECK(strcmp(buf, expect) == 0);
    CHECK_EQ((unsigned long)(end - buf), 10u);
    CHECK_EQ((unsigned char)*end, 0u);
    CHECK(guard_ok(11u));
}

int main(void)
{
    /* ---- u32_to_str: no padding, no sign, the two limits ---- */
    check_str(0u, "0");
    check_str(1u, "1");
    check_str(9u, "9");
    check_str(10u, "10");                       /* first two-digit value */
    check_str(255u, "255");
    check_str(65535u, "65535");
    check_str(65536u, "65536");
    check_str(100000u, "100000");               /* a zero inside the number */
    check_str(1000000u, "1000000");
    check_str(2147483648u, "2147483648");       /* bit 31 set, no sign shown */
    check_str(4294967295u, "4294967295");       /* 0xFFFFFFFF: 10 digits, the longest */
    check_str(4000000000u, "4000000000");       /* trailing zeros survive */

    /* ---- u32_to_hex: fixed width, "0x" + 8 upper-case digits ---- */
    check_hex(0u, "0x00000000");
    check_hex(0xFFFFFFFFu, "0xFFFFFFFF");
    check_hex(0xAu, "0x0000000A");              /* padded, upper case */
    check_hex(0x12345678u, "0x12345678");
    check_hex(0xDEADBEEFu, "0xDEADBEEF");
    check_hex(0x80000000u, "0x80000000");
    check_hex(0x0000ABCDu, "0x0000ABCD");

    /* ---- copy_str: whole string + terminator, nothing more ---- */
    fill();
    {
        char *end = copy_str(buf, "");
        CHECK(end == buf);                      /* empty: only the terminator */
        CHECK_EQ((unsigned char)buf[0], 0u);
        CHECK(guard_ok(1u));
    }
    fill();
    {
        char *end = copy_str(buf, " min=");
        CHECK(strcmp(buf, " min=") == 0);
        CHECK_EQ((unsigned long)(end - buf), 5u);
        CHECK(guard_ok(6u));
    }
    /* copy_str has no length parameter and never truncates: a string of
     * BUF - 1 characters fills the buffer exactly, terminator on the last
     * byte. (A longer string would overrun - the callers size their
     * buffers from the longest possible line, see cli.c.) */
    fill();
    {
        char longest[BUF];
        memset(longest, 'x', BUF - 1);
        longest[BUF - 1] = '\0';
        char *end = copy_str(buf, longest);
        CHECK(strcmp(buf, longest) == 0);
        CHECK_EQ((unsigned long)(end - buf), (unsigned long)(BUF - 1));
        CHECK_EQ((unsigned char)buf[BUF - 1], 0u);
    }

    /* ---- chaining, as console_half_stats() in cli.c does it ---- */
    fill();
    {
        char *p = copy_str(buf, "[half] n=");
        p = u32_to_str(p, 7u);
        p = copy_str(p, " min=");  p = u32_to_str(p, 0u);
        p = copy_str(p, " max=");  p = u32_to_hex(p, 0xFFFu);
        copy_str(p, "\r\n");
        const char *expect = "[half] n=7 min=0 max=0x00000FFF\r\n";
        CHECK(strcmp(buf, expect) == 0);
        CHECK(guard_ok(strlen(expect) + 1u));
    }

    /* console_kv()'s `char num[16]`: the longest value plus CRLF fits. */
    fill();
    {
        char *p = u32_to_str(buf, 4294967295u);
        p = copy_str(p, "\r\n");
        CHECK_EQ((unsigned long)(p - buf), 12u);   /* 10 + 2 < 16 */
        CHECK(strcmp(buf, "4294967295\r\n") == 0);
    }

    return check_summary();
}

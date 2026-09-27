/*
 * test_stats.c - host-side test for lib/stats.c (half_stats, half_mean)
 *
 * Built by tools\hosttest.bat with the installed MinGW gcc, against the
 * real src/lib/stats.c - not a reimplementation. Every expected value is
 * computed by hand in the comment next to it.
 *
 * The mean is `acc / n` with unsigned integer division, i.e. it TRUNCATES
 * (1.75 -> 1), it does not round. The tests below pin that down, because
 * a later "improvement" to rounding would move the [half] line and the
 * selftest's mean by one count.
 */
#include <stdint.h>
#include <stddef.h>

#include "stats.h"
#include "check.h"

/* Run half_stats() and half_mean() over one array and check all four
 * results; half_mean() must agree with half_stats()'s mean, since the
 * selftest (capture.c) uses the one and the console the other. */
static void check_block(const uint16_t *b, uint32_t n,
                        uint32_t e_min, uint32_t e_max, uint32_t e_mean)
{
    uint32_t mn = 12345u, mx = 12345u, mean = 12345u;
    half_stats(b, n, &mn, &mx, &mean);
    CHECK_EQ(mn, e_min);
    CHECK_EQ(mx, e_max);
    CHECK_EQ(mean, e_mean);
    CHECK_EQ(half_mean(b, n), e_mean);
}

int main(void)
{
    /* ---- n = 1: min = max = mean = the sample ---- */
    { static const uint16_t b[] = { 2048u };
      check_block(b, 1u, 2048u, 2048u, 2048u); }
    { static const uint16_t b[] = { 0u };
      check_block(b, 1u, 0u, 0u, 0u); }
    { static const uint16_t b[] = { 0xFFFFu };
      check_block(b, 1u, 0xFFFFu, 0xFFFFu, 0xFFFFu); }

    /* ---- all equal ---- */
    { static const uint16_t b[] = { 3840u, 3840u, 3840u, 3840u, 3840u, 3840u, 3840u, 3840u };
      check_block(b, 8u, 3840u, 3840u, 3840u); }
    { static const uint16_t b[] = { 0u, 0u, 0u };
      check_block(b, 3u, 0u, 0u, 0u); }
    /* 4 x 0xFFFF: acc = 262140, / 4 = 65535 - the sum passes 16 bits and
     * acc is 32 bits wide, so it must not wrap. */
    { static const uint16_t b[] = { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu };
      check_block(b, 4u, 0xFFFFu, 0xFFFFu, 0xFFFFu); }

    /* ---- both extremes in one block ---- */
    /* {0, 0xFFFF}: min 0, max 65535, acc 65535 / 2 = 32767.5 -> 32767 */
    { static const uint16_t b[] = { 0u, 0xFFFFu };
      check_block(b, 2u, 0u, 0xFFFFu, 32767u); }
    /* {0xFFFF, 0}: the same, order must not matter */
    { static const uint16_t b[] = { 0xFFFFu, 0u };
      check_block(b, 2u, 0u, 0xFFFFu, 32767u); }
    /* 12-bit full scale, extremes in the middle: min 0 at [2], max 4095
     * at [1]; acc = 100 + 4095 + 0 + 4095 + 100 = 8390, / 5 = 1678 */
    { static const uint16_t b[] = { 100u, 4095u, 0u, 4095u, 100u };
      check_block(b, 5u, 0u, 4095u, 1678u); }

    /* ---- a hand-computed ramp ---- */
    /* 10..19: min 10, max 19, acc = 145, / 10 = 14.5 -> 14 */
    { static const uint16_t b[] = { 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u };
      check_block(b, 10u, 10u, 19u, 14u); }
    /* the same ramp descending: identical results */
    { static const uint16_t b[] = { 19u, 18u, 17u, 16u, 15u, 14u, 13u, 12u, 11u, 10u };
      check_block(b, 10u, 10u, 19u, 14u); }
    /* min first, max last, and the reverse */
    { static const uint16_t b[] = { 5u, 500u, 50u, 5000u };     /* acc 5555 / 4 = 1388.75 -> 1388 */
      check_block(b, 4u, 5u, 5000u, 1388u); }
    { static const uint16_t b[] = { 5000u, 50u, 500u, 5u };
      check_block(b, 4u, 5u, 5000u, 1388u); }

    /* ---- the rounding, exactly as implemented: truncation ---- */
    /* {1, 2}: 1.5 -> 1 (round-half-up would give 2) */
    { static const uint16_t b[] = { 1u, 2u };
      check_block(b, 2u, 1u, 2u, 1u); }
    /* {1, 2, 2, 2}: 7 / 4 = 1.75 -> 1 (rounding to nearest would give 2) */
    { static const uint16_t b[] = { 1u, 2u, 2u, 2u };
      check_block(b, 4u, 1u, 2u, 1u); }
    /* {0, 0, 0, 1}: 0.25 -> 0 */
    { static const uint16_t b[] = { 0u, 0u, 0u, 1u };
      check_block(b, 4u, 0u, 1u, 0u); }
    /* {2, 3, 3}: 8 / 3 = 2.666.. -> 2 */
    { static const uint16_t b[] = { 2u, 3u, 3u };
      check_block(b, 3u, 2u, 3u, 2u); }
    /* {4095, 4094, 4094}: 12283 / 3 = 4094.333.. -> 4094 */
    { static const uint16_t b[] = { 4095u, 4094u, 4094u };
      check_block(b, 3u, 4094u, 4095u, 4094u); }
    /* n = 7, sum 4096: 585.14.. -> 585 */
    { static const uint16_t b[] = { 585u, 585u, 585u, 585u, 585u, 585u, 586u };
      check_block(b, 7u, 585u, 586u, 585u); }

    /* ---- only the first n samples count ---- */
    /* the block holds 6 values, n = 3 sees {7, 9, 8}: 24 / 3 = 8 */
    { static const uint16_t b[] = { 7u, 9u, 8u, 0u, 0xFFFFu, 1u };
      check_block(b, 3u, 7u, 9u, 8u); }

    /* ---- a half-sized block, as the firmware uses it (2048 samples) ---- */
    /* b[i] = i for i = 0..2047: min 0, max 2047, acc = 2047 * 2048 / 2 =
     * 2096128, / 2048 = 1023.5 -> 1023 */
    {
        static uint16_t b[2048];
        for (uint32_t i = 0; i < 2048u; i++) { b[i] = (uint16_t)i; }
        check_block(b, 2048u, 0u, 2047u, 1023u);
    }
    /* 2048 x 4095 = 8386560 - beyond 16 bits, well within 32 */
    {
        static uint16_t b[2048];
        for (uint32_t i = 0; i < 2048u; i++) { b[i] = 4095u; }
        check_block(b, 2048u, 4095u, 4095u, 4095u);
    }

    return check_summary();
}

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
 * test_iir1.c - host-side test for lib/iir1.c (P3.2)
 *
 * Built by tools\hosttest.bat with the installed MinGW gcc against the
 * real src/lib/iir1.c. The expected values come from the reference
 * model tests/ref/goertzel_ref.py (its `iir1` subcommand), which models
 * the template's integer arithmetic EXACTLY - Python ints with floor
 * shifts, i.e. C int32 arithmetic shifts - so every comparison below is
 * exact, not a tolerance:
 *
 *   iir1_step_k{2,4,6}.csv  0, +1000, -1000, 0 steps; columns x, lp, hp
 *   iir1_limit_k4.csv       the same steps at +-(2^27 - 1), the largest
 *                           input the int32 tap takes at k = 4 (the tap
 *                           reaches +-(2^31 - 16)); a match here proves
 *                           the C tap does not overflow at the limit.
 *
 * Then the point of the module: instances are independent. Two filters
 * fed with different signals, interleaved sample by sample, each produce
 * exactly what they produce alone, and a reset of one leaves the other
 * untouched - the template's FLT_vIIR_Init() cleared all filters at once,
 * which is the bug that made lib/iir1 necessary (DESIGN-MULTICHANNEL.md
 * 4.3).
 */
#include <stdint.h>
#include <stdio.h>

#include "iir1.h"
#include "check.h"
#include "vectors.h"

/* Exact compare of two int32 with the row number in the message. */
static void expect_i32(const char *what, int row, int32_t got, int32_t want)
{
    check_total_++;
    if (got != want) {
        check_failed_++;
        fprintf(stderr, "%s row %d: got %ld, want %ld\n",
                what, row, (long)got, (long)want);
    }
}

/* One vector file against one LP instance and one HP instance of that k. */
static void check_vector(const char *argv0, const char *name, uint8_t k)
{
    FILE *f = vectors_open(argv0, name);
    CHECK(f != NULL);
    if (f == NULL) { return; }

    iir1_t lp, hp;
    iir1_init(&lp, k);
    iir1_init(&hp, k);
    CHECK_EQ(lp.k, k);
    CHECK_EQ(lp.tap, 0);

    double v[3];
    int row = 0, r;
    while ((r = vectors_row(f, v, 3)) == 1) {
        const int32_t x = (int32_t)v[0];
        expect_i32(name, row, iir1_lp(&lp, x), (int32_t)v[1]);
        expect_i32(name, row, iir1_hp(&hp, x), (int32_t)v[2]);
        row++;
    }
    CHECK_EQ(r, 0);         /* ended at EOF, not on a bad row */
    CHECK_EQ(row, 128);     /* the files hold 128 rows */
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *argv0 = argc > 0 ? argv[0] : NULL;

    /* ---- step responses, exact, three values of k and the int32 limit ---- */
    check_vector(argv0, "iir1_step_k2.csv", 2u);
    check_vector(argv0, "iir1_step_k4.csv", 4u);
    check_vector(argv0, "iir1_step_k6.csv", 6u);
    check_vector(argv0, "iir1_limit_k4.csv", 4u);

    /* ---- a few values by hand, so the vectors are not the only witness ---- */
    /* k = 4, x = 1000 from rest: tap = 0 - 0 + 1000 = 1000, y = 1000 >> 4 = 62;
     * next: tap = 1000 - 62 + 1000 = 1938, y = 121 (the CSV's first rows). */
    {
        iir1_t f;
        iir1_init(&f, 4u);
        CHECK_EQ(iir1_lp(&f, 1000), 62);
        CHECK_EQ(f.tap, 1000);
        CHECK_EQ(iir1_lp(&f, 1000), 121);
        CHECK_EQ(f.tap, 1938);
        /* high-pass from rest, x = 1000: tap = 1000, y = 1000 - 62 = 938 */
        iir1_reset(&f);
        CHECK_EQ(f.k, 4u);              /* reset keeps k */
        CHECK_EQ(iir1_hp(&f, 1000), 938);
        /* a negative input: tap = 1000 - 62 + (-1000) = -62; -62 >> 4 is
         * -4 (arithmetic shift, floor), so y = -1000 - (-4) = -996 */
        CHECK_EQ(iir1_hp(&f, -1000), -996);
        CHECK_EQ(f.tap, -62);
    }
    /* k = 1: the tap halves and adds; DC gain of the tap is 2 */
    {
        iir1_t f;
        iir1_init(&f, 1u);
        CHECK_EQ(iir1_lp(&f, 100), 50);     /* tap 100 */
        CHECK_EQ(iir1_lp(&f, 100), 75);     /* tap 100 - 50 + 100 = 150 */
        CHECK_EQ(iir1_lp(&f, 100), 87);     /* tap 150 - 75 + 100 = 175 */
        CHECK_EQ(iir1_lp(&f, 100), 94);     /* tap 175 - 87 + 100 = 188 */
    }

    /* ---- independence: two instances, interleaved, vs. each alone ---- */
    {
        enum { N = 200 };
        static int32_t xa[N], xb[N], ya_alone[N], yb_alone[N];
        for (int i = 0; i < N; i++) {
            xa[i] = (i < 100) ? 1000 : -1000;           /* a step down */
            xb[i] = (i % 7) * 300 - 900;                /* a saw, -900..900 */
        }
        iir1_t a, b;
        iir1_init(&a, 4u);
        for (int i = 0; i < N; i++) { ya_alone[i] = iir1_lp(&a, xa[i]); }
        iir1_init(&b, 3u);
        for (int i = 0; i < N; i++) { yb_alone[i] = iir1_hp(&b, xb[i]); }

        /* interleaved, b reset half way: a must not notice */
        iir1_init(&a, 4u);
        iir1_init(&b, 3u);
        int b_reset_seen = 0;
        for (int i = 0; i < N; i++) {
            int32_t ya = iir1_lp(&a, xa[i]);
            int32_t yb = iir1_hp(&b, xb[i]);
            expect_i32("interleaved a", i, ya, ya_alone[i]);
            if (i < 100) {
                expect_i32("interleaved b", i, yb, yb_alone[i]);
            } else if (i == 100) {
                /* b was reset before this sample: from rest, hp(x) =
                 * x - (x >> 3) */
                expect_i32("b after reset", i, yb, xb[i] - (xb[i] >> 3));
                b_reset_seen = 1;
            }
            if (i == 99) { iir1_reset(&b); }
        }
        CHECK(b_reset_seen);
        /* and a's state is exactly what a alone would hold */
        iir1_t a2;
        iir1_init(&a2, 4u);
        for (int i = 0; i < N; i++) { (void)iir1_lp(&a2, xa[i]); }
        CHECK_EQ(a.tap, a2.tap);
    }

    return check_summary();
}

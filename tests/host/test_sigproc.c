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
 * test_sigproc.c - src/core/sigproc.c's 4th-order Butterworth low-pass at
 * fs/8 (02.10.2026; fs/4 that morning) on a host gcc: the gain at DC, in
 * the pass band, at the cut-off and in the stop band against the design (the numbers in
 * sigproc.c's comment), no seam between blocks, the re-start on a gap, and
 * the clamp to 0..4095.
 * Since 02.10.2026 also the selectable filters - high-pass and band-pass
 * at fs/8 against the analog Butterworth magnitude, computed here from the
 * formula, not from sigproc.c's coefficients (tools/sigproc_design.py made
 * those) - the mid-scale offset of both, the switch between filters, and
 * the Goertzel at fs/16: amplitude, share, detection, its threshold, that
 * it sees the input before the filter, and that a DC level does not leak
 * into it at a block length that is no multiple of 16.
 */
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "sigproc.h"
#include "check.h"

#define N      2048u
#define MID    2048.0
#define AMP    1000.0
#define PI     3.14159265358979323846

static uint16_t buf[N];

static void sine(uint16_t *b, uint32_t n, double f, uint32_t start)
{
    for (uint32_t i = 0; i < n; i++) {
        b[i] = (uint16_t)lround(MID + AMP * sin(2.0 * PI * f * (double)(start + i) + 0.3));
    }
}

/* Amplitude of b[from..n-1] around MID: sqrt(2) x RMS. */
static double amplitude(const uint16_t *b, uint32_t from, uint32_t n)
{
    double acc = 0.0;
    for (uint32_t i = from; i < n; i++) { const double d = (double)b[i] - MID; acc += d * d; }
    return sqrt(2.0 * acc / (double)(n - from));
}

static void run(uint16_t *b, uint32_t n, bool gap, uint32_t seq)
{
    const sigproc_info_t info = { seq & 1u, seq, 0u, gap ? 1u : 0u };
    sigproc_block(b, n, &info);
}

/* The gain of one sine through the filter, settled (the first 256 left out). */
static double gain_at(double f)
{
    sine(buf, N, f, 0u);
    run(buf, N, true, 1u);
    return amplitude(buf, 256u, N) / AMP;
}

/* |H| of the analog prototype at the pre-warped frequency f (units of fs). */
static double analog(int kind, double f)
{
    const double w = tan(PI * f), wc = tan(PI / 8.0);
    if (kind == 1) { return 1.0 / sqrt(1.0 + pow(w / wc, 8.0)); }
    if (kind == 2) { return 1.0 / sqrt(1.0 + pow(wc / w, 8.0)); }
    const double w1 = tan(PI / 8.0 / sqrt(2.0)), w2 = tan(PI / 8.0 * sqrt(2.0));
    const double x = (w * w - w1 * w2) / ((w2 - w1) * w);
    return 1.0 / sqrt(1.0 + pow(x, 4.0));
}

/* A sine of amplitude amp at frequency f around level dc, length n. */
static void tone(uint16_t *b, uint32_t n, double f, double amp, double dc)
{
    for (uint32_t i = 0; i < n; i++) {
        b[i] = (uint16_t)lround(dc + amp * sin(2.0 * PI * f * (double)i + 0.7));
    }
}

int main(void)
{
    sigproc_set_filter(SIGPROC_LP);      /* off after reset since 02.10.2026 */
    /* ---- DC passes unchanged, from the very first sample (settled state) */
    for (uint32_t i = 0; i < N; i++) { buf[i] = 2000u; }
    run(buf, N, true, 1u);
    {
        bool all = true;
        for (uint32_t i = 0; i < N; i++) { if (buf[i] != 2000u) { all = false; } }
        CHECK(all);
    }

    /* ---- pass band, cut-off, stop band: |H| per sigproc.c's comment ---- */
    const double g05 = gain_at(0.05), g10 = gain_at(0.10), g125 = gain_at(0.125);
    const double g15 = gain_at(0.15), g20 = gain_at(0.20), g25 = gain_at(0.25);
    CHECK(fabs(g05 - 0.9998) < 0.005);           /* 0 dB                       */
    CHECK(fabs(g10 - 0.9352) < 0.005);           /* -0.58 dB                   */
    CHECK(fabs(g125 - 0.70711) < 0.005);         /* -3.01 dB: the cut-off fs/8 */
    CHECK(fabs(g15 - 0.4002) < 0.005);           /* -7.95 dB                   */
    CHECK(fabs(g20 - 0.1051) < 0.003);           /* -19.6 dB                   */
    CHECK(fabs(g25 - 0.0294) < 0.003);           /* -30.6 dB (rounding: 0.5 LSB) */

    /* ---- no seam: two blocks in a row = one block of twice the length ---- */
    {
        static uint16_t one[N], two[N];
        sine(one, N, 0.07, 0u);
        for (uint32_t i = 0; i < N; i++) { two[i] = one[i]; }
        run(one, N, true, 10u);                              /* one block      */
        run(two, N / 2u, true, 20u);                         /* two halves     */
        run(two + N / 2u, N / 2u, false, 21u);
        bool same = true;
        for (uint32_t i = 0; i < N; i++) { if (one[i] != two[i]) { same = false; } }
        CHECK(same);
    }

    /* ---- a gap re-starts the state at the block's first sample: a
     *      constant block after a sine comes out constant ---- */
    sine(buf, N, 0.1, 0u);
    run(buf, N, true, 30u);
    for (uint32_t i = 0; i < N; i++) { buf[i] = 1234u; }
    run(buf, N, true, 40u);                                  /* gap: no carry  */
    {
        bool all = true;
        for (uint32_t i = 0; i < N; i++) { if (buf[i] != 1234u) { all = false; } }
        CHECK(all);
    }

    /* ---- the clamp: a full-scale step overshoots (about 10 %), and is
     *      held at 4095 - never wraps to a small value ---- */
    for (uint32_t i = 0; i < N; i++) { buf[i] = (i < 64u) ? 0u : 4095u; }
    run(buf, N, true, 50u);
    {
        uint32_t mx = 0u, low_after = 0u;
        for (uint32_t i = 0; i < N; i++) {
            if (buf[i] > mx) { mx = buf[i]; }
            if ((i > 80u) && (buf[i] < 3500u)) { low_after++; }
        }
        CHECK_EQ(mx, 4095u);
        CHECK_EQ(low_after, 0u);
        CHECK_EQ(buf[N - 1u], 4095u);
    }

    /* ---- high-pass and band-pass at fs/8 against the analog magnitude ---- */
    {
        static const double fr[] = { 0.03, 0.0625, 0.0884, 0.10, 0.125, 0.15, 0.1768, 0.20, 0.25, 0.35 };
        for (int kind = 2; kind <= 3; kind++) {
            sigproc_set_filter((sigproc_filter_t)kind);
            for (uint32_t k = 0; k < sizeof fr / sizeof fr[0]; k++) {
                const double g = gain_at(fr[k]), want = analog(kind, fr[k]);
                if (fabs(g - want) >= 0.004) {
                    fprintf(stderr, "kind %d f %.4f: %.4f want %.4f\n", kind, fr[k], g, want);
                }
                CHECK(fabs(g - want) < 0.004);
            }
        }
        /* the low-pass, through the same formula */
        sigproc_set_filter(SIGPROC_LP);
        CHECK(fabs(gain_at(0.1768) - analog(1, 0.1768)) < 0.004);
    }

    /* ---- hp/bp block DC: a constant comes out at mid-scale, from the
     *      first sample (settled), and so does a fresh filter switch ---- */
    for (int kind = 2; kind <= 3; kind++) {
        sigproc_set_filter((sigproc_filter_t)kind);
        for (uint32_t i = 0; i < N; i++) { buf[i] = 3000u; }
        run(buf, N, false, 60u + (uint32_t)kind);      /* no gap: the switch restarts it */
        bool all = true;
        for (uint32_t i = 0; i < N; i++) { if (buf[i] != 2048u) { all = false; } }
        CHECK(all);
    }

    /* ---- off: the block passes unchanged ---- */
    sigproc_set_filter(SIGPROC_OFF);
    sine(buf, N, 0.2, 0u);
    {
        static uint16_t ref[N];
        for (uint32_t i = 0; i < N; i++) { ref[i] = buf[i]; }
        run(buf, N, true, 70u);
        bool same = true;
        for (uint32_t i = 0; i < N; i++) { if (buf[i] != ref[i]) { same = false; } }
        CHECK(same);
    }
    CHECK(!sigproc_active());

    /* ---- Goertzel at fs/16 ---- */
    sigproc_set_goertzel(true);
    CHECK(sigproc_active());
    {
        sigproc_gz_t gz;
        sigproc_goertzel_get(&gz);
        CHECK_EQ(gz.valid, 0u);                         /* nothing seen yet   */

        tone(buf, 1024u, 1.0 / 16.0, 800.0, 2000.0);    /* the tone, n = 64 x 16 */
        run(buf, 1024u, true, 80u);
        sigproc_goertzel_get(&gz);
        CHECK_EQ(gz.valid, 1u);
        CHECK(gz.amp >= 798u && gz.amp <= 802u);
        CHECK(gz.rms >= 563u && gz.rms <= 569u);        /* 800 / sqrt2 = 566 */
        CHECK(gz.share_pm >= 995u);
        CHECK_EQ(gz.detected, 1u);
        CHECK_EQ(gz.thr, 100u);
        CHECK_EQ(gz.seq, 80u);

        tone(buf, 1024u, 1.0 / 8.0, 800.0, 2000.0);     /* another tone: no   */
        run(buf, 1024u, true, 81u);
        sigproc_goertzel_get(&gz);
        CHECK(gz.amp <= 2u);
        CHECK(gz.share_pm <= 2u);
        CHECK_EQ(gz.detected, 0u);

        /* a block length that is no multiple of 16 (1000): the 2000-LSB DC
         * level must not leak into the bin - the mean is taken out first */
        for (uint32_t i = 0; i < 1000u; i++) { buf[i] = 2000u; }
        run(buf, 1000u, true, 82u);
        sigproc_goertzel_get(&gz);
        CHECK(gz.amp <= 1u);
        CHECK_EQ(gz.detected, 0u);

        /* the threshold */
        sigproc_set_threshold(900u);
        tone(buf, 1024u, 1.0 / 16.0, 800.0, 2000.0);
        run(buf, 1024u, true, 83u);
        sigproc_goertzel_get(&gz);
        CHECK_EQ(gz.thr, 900u);
        CHECK_EQ(gz.detected, 0u);
        sigproc_set_threshold(100u);

        /* the Goertzel sees the input before the filter: with the
         * high-pass on (|H(fs/16)| = 0.053) the tone still reads 800 */
        sigproc_set_filter(SIGPROC_HP);
        tone(buf, 1024u, 1.0 / 16.0, 800.0, 2000.0);
        run(buf, 1024u, true, 84u);
        sigproc_goertzel_get(&gz);
        CHECK(gz.amp >= 798u && gz.amp <= 802u);
        CHECK(amplitude(buf, 256u, 1024u) < 0.06 * 800.0 + 3.0);   /* and the filter did run */
    }
    /* ---- the folded computation (02.10.2026) against a double-precision
     *      DFT at fs/16 straight from the definition: block lengths that are
     *      and are not multiples of 16, a tone off the bin, noise, a level
     *      near full scale - amplitude, rms and share to the rounding ---- */
    sigproc_set_filter(SIGPROC_OFF);
    {
        static const uint32_t ns[] = { 16u, 17u, 100u, 999u, 1000u, 1024u };
        uint32_t lcg = 12345u;
        for (uint32_t t = 0; t < sizeof ns / sizeof ns[0]; t++) {
            const uint32_t n = ns[t];
            for (uint32_t i = 0; i < n; i++) {
                lcg = lcg * 1103515245u + 12345u;
                const double noise = (double)((lcg >> 16) % 201u) - 100.0;
                double v = 3900.0 + 150.0 * sin(2.0 * PI * 0.0611 * (double)i) + noise
                           + 30.0 * sin(2.0 * PI * (double)i / 16.0 + 0.4);
                if (t & 1u) { v -= 3000.0; }
                buf[i] = (uint16_t)lround(v < 0.0 ? 0.0 : (v > 4095.0 ? 4095.0 : v));
            }
            double m = 0.0;
            for (uint32_t i = 0; i < n; i++) { m += buf[i]; }
            m /= (double)n;
            double re = 0.0, im = 0.0, var = 0.0;
            for (uint32_t i = 0; i < n; i++) {
                const double d = (double)buf[i] - m;
                re += d * cos(2.0 * PI * (double)i / 16.0);
                im -= d * sin(2.0 * PI * (double)i / 16.0);
                var += d * d;
            }
            var /= (double)n;
            const double amp = 2.0 * sqrt(re * re + im * im) / (double)n;
            run(buf, n, true, 90u + t);
            sigproc_gz_t gz;
            sigproc_goertzel_get(&gz);
            if (fabs((double)gz.amp - amp) > 0.51 || fabs((double)gz.rms - sqrt(var)) > 0.51) {
                fprintf(stderr, "n %u: amp %u want %.3f, rms %u want %.3f\n", n, gz.amp, amp, gz.rms, sqrt(var));
            }
            CHECK(fabs((double)gz.amp - amp) <= 0.51);
            CHECK(fabs((double)gz.rms - sqrt(var)) <= 0.51);
            CHECK(fabs((double)gz.share_pm - 1000.0 * amp * amp * 0.5 / var) <= 1.5);
        }
    }

    sigproc_set_goertzel(false);
    sigproc_set_filter(SIGPROC_OFF);
    CHECK(!sigproc_active());

    return check_summary();
}

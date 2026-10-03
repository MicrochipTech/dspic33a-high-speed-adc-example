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
 * test_impact.c - src/app/impact.c's impact counter (CNT, 02.10.2026) on a
 * host gcc, through the example's own sigproc_block() with every filter
 * off: impact.c defines sigproc.h's application hooks strongly, so this is
 * also the check that they reach it. Moved out of test_sigproc.c with the
 * counter itself (02.10.2026), the cases and numbers unchanged: a steady
 * tone reads its amplitude and counts once, a reset in the middle of it
 * does not count again, 40 rings at 1000/s, amplitudes spread 1:3, two
 * rings 100 us apart merge, 5000 rings a second on each other's tails,
 * rings at the wrong frequency stay below the threshold. And the console
 * part: "sigproc cnt ..." handled, other sub-commands left to cli.c, the
 * GRAB fields only while counting and only when they fit.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "sigproc.h"
#include "console.h"
#include "gui_link.h"
#include "impact.h"
#include "check.h"

#define PI     3.14159265358979323846

/* cli.c's reply helpers, which impact.c borrows: recorded, not printed. */
static char out[2048];
static void out_add(const char *a, const char *b)
{
    strncat(out, a, sizeof(out) - strlen(out) - 1u);
    strncat(out, b, sizeof(out) - strlen(out) - 1u);
    strncat(out, "\n", sizeof(out) - strlen(out) - 1u);
}
void put_kv(const char *key, uint32_t v)
{
    char num[16];
    snprintf(num, sizeof(num), ": %u", (unsigned)v);
    out_add(key, num);
}
void put_kv_str(const char *key, const char *s)
{
    char val[32];
    snprintf(val, sizeof(val), ": %s", s);
    out_add(key, val);
}
void usage(const char *text) { out_add("usage: ", text); }
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *v)
{
    unsigned long x = 0;
    if (sscanf(s, "%lu", &x) != 1 || x < lo || x > hi) { return false; }
    *v = (uint32_t)x;
    return true;
}

static int cmd(const char *line)
{
    static char buf[64];
    char *argv[8];
    int argc = 0;
    strncpy(buf, line, sizeof(buf) - 1u);
    for (char *t = strtok(buf, " "); t != NULL && argc < 8; t = strtok(NULL, " ")) { argv[argc++] = t; }
    out[0] = '\0';
    return sigproc_app_cmd(argc, argv);
}

int main(void)
{
    sigproc_set_filter(SIGPROC_OFF);
    sigproc_set_goertzel(false);
    CHECK(!sigproc_active());

    /* ---- the impact counter (CNT, 02.10.2026) ---- */
    {
        static uint16_t sig[48000];
        const double fs = 1.0e6;
        impact_t c;
        sigproc_set_fs((float)fs);
        CHECK(!impact_config(450000u, 100u, 150u));          /* above fs / 2.5 */
        CHECK(!impact_config(50000u, 1u, 150u));             /* tau too small  */
        CHECK(impact_config(50000u, 100u, 150u));
        impact_enable(true);
        CHECK(sigproc_active());

        /* bursts: amplitude a[k], decaying with 100 us, at f, starting at t0[k] */
        #define CNT_MAXB 64
        static double t0[CNT_MAXB], am[CNT_MAXB], fr[CNT_MAXB];
        uint32_t nb = 0u;
        uint32_t seqc = 1000u;
        #define CNT_SYNTH(LEN) do { \
            for (uint32_t i_ = 0; i_ < (LEN); i_++) { \
                double v_ = 2048.0; \
                for (uint32_t b_ = 0; b_ < nb; b_++) { \
                    const double tt_ = ((double)i_ - t0[b_]) / fs; \
                    if (tt_ >= 0.0 && tt_ < 2.0e-3) { v_ += am[b_] * exp(-tt_ / 100.0e-6) * sin(2.0 * PI * fr[b_] * tt_); } \
                } \
                sig[i_] = (uint16_t)lround(v_ < 0.0 ? 0.0 : (v_ > 4095.0 ? 4095.0 : v_)); \
            } } while (0)
        /* feed in blocks of 1024 (not aligned to anything), no gaps */
        /* quiet input: the previous scenario's ring dies away first */
        #define CNT_QUIET() do { nb = 0u; for (uint32_t i_ = 0; i_ < 4096u; i_++) { sig[i_] = 2048u; } CNT_FEED(4096u); } while (0)
        #define CNT_FEED(LEN) do { \
            for (uint32_t o_ = 0; o_ < (LEN); o_ += 1024u) { \
                const uint32_t m_ = ((LEN) - o_ < 1024u) ? ((LEN) - o_) : 1024u; \
                static uint16_t blk_[1024]; \
                for (uint32_t i_ = 0; i_ < m_; i_++) { blk_[i_] = sig[o_ + i_]; } \
                const sigproc_info_t inf_ = { 0u, seqc, 0u, 0u }; \
                seqc++; \
                sigproc_block(blk_, m_, &inf_); \
            } } while (0)

        /* a steady tone of 800 LSB reads 800, and counts once */
        nb = 0u;
        for (uint32_t i = 0; i < 16384u; i++) {
            sig[i] = (uint16_t)lround(2048.0 + 800.0 * sin(2.0 * PI * 50000.0 * (double)i / fs));
        }
        impact_reset();
        CNT_FEED(16384u);
        impact_get(&c, true);
        if (c.peak < 790u || c.peak > 810u) { fprintf(stderr, "steady peak %u\n", c.peak); }
        CHECK(c.peak >= 790u && c.peak <= 810u);
        CHECK_EQ(c.count, 1u);
        CHECK_EQ(c.fs_hz, 1000000u);
        /* a reset in the middle of that tone does not count it again */
        impact_reset();
        CNT_FEED(16384u);
        impact_get(&c, true);
        CHECK_EQ(c.count, 0u);

        /* 40 rings of 1000 LSB, 1 ms apart (1000/s), across 1024-sample
         * blocks: 40 counted, and the rate 40 over the time seen */
        CNT_QUIET();
        nb = 0u;
        for (uint32_t k = 0; k < 40u; k++) { t0[nb] = 300.0 + 1000.0 * k; am[nb] = 1000.0; fr[nb] = 50000.0; nb++; }
        impact_reset();
        CNT_SYNTH(41000u);
        CNT_FEED(41000u);
        impact_get(&c, true);
        if (c.count != 40u) { fprintf(stderr, "train: %u\n", c.count); }
        CHECK_EQ(c.count, 40u);
        CHECK_EQ(c.ms, 41u);
        CHECK(c.rate >= 970u && c.rate <= 980u);              /* 40 / 0.041 s = 976 */
        CHECK(c.peak >= 340u && c.peak <= 400u);              /* 368 computed      */

        /* amplitudes spread 1:3 (700..2000 LSB): all counted at thr 150 */
        CNT_QUIET();
        nb = 0u;
        for (uint32_t k = 0; k < 30u; k++) { t0[nb] = 500.0 + 1300.0 * k; am[nb] = 700.0 + 1300.0 * (double)((k * 7u) % 11u) / 10.0; fr[nb] = 50000.0; nb++; }
        impact_reset();
        CNT_SYNTH(40000u);
        CNT_FEED(40000u);
        impact_get(&c, true);
        CHECK_EQ(c.count, 30u);

        /* two rings 100 us apart merge: counted once (the limit) */
        CNT_QUIET();
        nb = 0u;
        t0[nb] = 2000.0; am[nb] = 1000.0; fr[nb] = 50000.0; nb++;
        t0[nb] = 2100.0; am[nb] = 1000.0; fr[nb] = 50000.0; nb++;
        impact_reset();
        CNT_SYNTH(8192u);
        CNT_FEED(8192u);
        impact_get(&c, true);
        CHECK_EQ(c.count, 1u);

        /* 5000 rings a second (200 us apart, 100-us decay): each one on the
         * last one's tail - counted all the same (relative re-arm; until
         * the same evening the count stopped at 1 here) */
        CNT_QUIET();
        nb = 0u;
        for (uint32_t k = 0; k < 40u; k++) { t0[nb] = 300.0 + 200.0 * k; am[nb] = 1000.0; fr[nb] = 50000.0; nb++; }
        impact_reset();
        CNT_SYNTH(9000u);
        CNT_FEED(9000u);
        impact_get(&c, true);
        if (c.count < 39u || c.count > 40u) { fprintf(stderr, "5000/s: %u of 40\n", c.count); }
        CHECK(c.count >= 39u && c.count <= 40u);

        /* rings at 100 kHz, the same amplitude: 29 % of the peak (107 of
         * 368) - below thr 150, not counted */
        CNT_QUIET();
        nb = 0u;
        for (uint32_t k = 0; k < 20u; k++) { t0[nb] = 300.0 + 1000.0 * k; am[nb] = 1000.0; fr[nb] = 100000.0; nb++; }
        impact_reset();
        CNT_SYNTH(21000u);
        CNT_FEED(21000u);
        impact_get(&c, true);
        if (c.count != 0u) { fprintf(stderr, "100 kHz: %u, peak %u\n", c.count, c.peak); }
        CHECK_EQ(c.count, 0u);
        CHECK(c.peak < 150u);

        /* off: nothing counted, the count kept */
        impact_enable(false);
        CHECK(!sigproc_active());
    }

    /* ---- the console part ---- */
    CHECK_EQ(cmd("sigproc lp"), 0);                       /* cli.c's own      */
    CHECK_EQ(cmd("sigproc cnt bogus"), -1);
    CHECK(strstr(out, "usage: ") != NULL);
    CHECK_EQ(cmd("sigproc cnt f 999"), -1);               /* below 1000 Hz    */
    CHECK_EQ(cmd("sigproc cnt f 20000"), 1);
    CHECK_EQ(cmd("sigproc cnt tau 50"), 1);
    CHECK_EQ(cmd("sigproc cnt thr 300"), 1);
    {
        impact_t c;
        impact_get(&c, false);
        CHECK_EQ(c.f_hz, 20000u);
        CHECK_EQ(c.tau_us, 50u);
        CHECK_EQ(c.thr, 300u);
    }
    CHECK_EQ(cmd("sigproc cnt on"), 1);
    CHECK(sigproc_active());
    out[0] = '\0';
    sigproc_app_status();
    CHECK(strstr(out, "cnt: on") != NULL);
    CHECK(strstr(out, "cnt_f_hz: 20000") != NULL);
    CHECK(strstr(out, "cnt_count: ") != NULL);

    /* GRAB fields: three while counting, nothing when they do not fit */
    {
        char head[96] = "GRAB n=1";
        char *e = gui_link_app_fields(head + 8, head + sizeof(head) - 3);
        CHECK(e > head + 8);
        CHECK(strstr(head, " cnt=") != NULL && strstr(head, " cnr=") != NULL && strstr(head, " cpk=") != NULL);
        char small[48] = "GRAB n=1";
        CHECK(gui_link_app_fields(small + 8, small + 30) == small + 8);
    }
    CHECK_EQ(cmd("sigproc cnt off"), 1);
    CHECK(!sigproc_active());
    {
        char head[96] = "GRAB";
        CHECK(gui_link_app_fields(head + 4, head + sizeof(head) - 3) == head + 4);
    }

    return check_summary();
}

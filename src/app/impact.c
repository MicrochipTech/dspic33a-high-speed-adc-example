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
 * impact.c - the impact counter, the customer application's processing:
 * it runs inside the example's signal processing through sigproc.h's
 * application hooks (sigproc_app_block() and its companions, strong here,
 * weak and empty in sigproc.c), adds its sub-commands to the console's
 * "sigproc" command (sigproc_app_cmd()/_status(), console.h) and its
 * fields to the GRAB header (gui_link_app_fields(), gui_link.h). Moved out
 * of src/core/sigproc.c on 02.10.2026, the algorithm unchanged, so the
 * example's core stays generic; the console commands, their replies and
 * the GRAB fields are the same as before.
 *
 * The impact counter (CNT, 02.10.2026, docs/IMPLEMENTATION-PLAN.md): balls
 * falling onto a metal plate, each impact a ring at f (20..200 kHz) that
 * dies away within 0.5 ms, up to about 1000 a second. Per sample, on the
 * input before the filter:
 *     d  = x[i] - x[i-1]                      (the first difference)
 *     q0 = d + 2 D cos(w) q1 - D^2 q2         (a damped Goertzel resonator)
 * with w = 2 pi f / fs and D = exp(-1 / (tau fs)), and every 4th sample the
 * magnitude |q1 - e^(-jw) q2| against the threshold: count once above thr,
 * re-arm below thr / 2. Two departures from lib/goertzel_f, which the plan
 * had named: (1) the first difference - the raw samples sit at about 2000
 * LSB, and that DC level puts a standing magnitude into the resonator of
 * the same order as a ring's (about 6500 for 50 kHz, tau 25 us, 1 MSPS);
 * the difference removes DC exactly, with no state of its own. (2) the
 * magnitude only every 4th sample, squared, against squared thresholds -
 * no square root, no low-pass, no per-sample magnitude buffer.
 *
 * The detector counts a ring when the magnitude is above thr AND has risen
 * by 1/0.7 from the lowest value since it was last re-armed; it re-arms
 * when the magnitude has fallen below 0.7 x the peak since the count (or
 * below thr / 2). Until the same evening it was lib/detect's rule - re-arm
 * only below thr / 2 - and at 5000 rings a second (200 us apart, 100-us
 * decay) the magnitude never got that low: the count stopped at 1 for
 * good (board, setup cnt_dense). Re-armed relative to the peak, a new ring
 * on top of the last one's tail is counted: in a model (02.10.2026)
 * regular rings were counted exactly up to 5000/s at tau 100 and up to
 * 10000/s at tau 25, and with random spacing (0.5..1.5) and amplitude
 * (0.3..1) 37 of 38 at 1000/s, 77 of 79 at 2000/s, then fewer and fewer -
 * too few, never stuck - as the rings crowd. On the board (same evening,
 * the generator's regular rings decaying with 100 us): exact at 1000..5000/s
 * with tau 100 (32501 of 32500 at 5000/s); 7000/s and 10000/s count 0 at
 * tau 100 - regular rings closer than the decay still merge - and exactly
 * at tau 25 (45048 of 45049, 64235 of 64240). Requiring the RISE matters:
 * re-arming alone, without it, counted every ring twice (its own tail is
 * still above thr when it re-arms). The magnitude is scaled so a
 * steady tone of amplitude A at f reads A (LSB): cnt_setup() computes the
 * chain's gain once per configuration (exactly, see there), so the
 * threshold is in the units of the signal, whatever f, tau and fs are.
 *
 * tau: best set to the ring's own decay time - the resonator is then the
 * matched filter for a damped sine. Default 100 us. What it can and cannot
 * do (numbers for a 1000-LSB ring at 50 kHz decaying with 100 us, 1 MSPS,
 * computed 02.10.2026 before building): its peak magnitude is 368 at tau
 * 100 (629 at 25, 250 at 200 - a short ring never reaches a steady tone's
 * full amplitude); it falls below a quarter of that 270 us after the peak,
 * so impacts 1 ms apart are counted one by one and ones closer than about
 * 0.3 ms merge. A ring at ANOTHER frequency still moves it - a short burst
 * is broadband: 19 % of the on-frequency peak at 25 kHz, 49 % at 40, 64 %
 * at 60, 39 % at 80, 29 % at 100 kHz. Telling the frequencies apart is
 * therefore a matter of the threshold sitting between those levels, about
 * 2.5:1 here, not an absolute property.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "sigproc.h"
#include "console.h"
#include "gui_link.h"
#include "fmt.h"
#include "impact.h"

/* cli.c's reply helpers (not static there; bench.c and cli_lab.c borrow
 * them the same way). */
void put_kv(const char *key, uint32_t v);
void put_kv_str(const char *key, const char *s);
bool arg_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out);
void usage(const char *text);

/* The counter's state. Configuration from the console,
 * applied at the next block (cnt_dirty): the console never runs while a
 * block is processed (cli.c's uart_rx_hook()). */
#define CNT_EVERY 4u                    /* the magnitude every 4th sample  */
#define CNT_DROP2 0.49f                 /* 0.7^2: re-arm at 70 % of the peak, count after a rise by 1/0.7 */
static struct {
    volatile bool on, dirty, reset;
    volatile uint32_t f_hz, tau_us, thr;
    volatile float fs;
    float c, dd, cw, sw;                /* 2 D cos w, D^2, cos w, sin w     */
    float thr2, rearm2, inv_scale2;     /* squared, in resonator units     */
    float q1, q2, xp;                   /* resonator, previous sample      */
    bool armed, ready;
    float pk2, tr2;                     /* peak since the count, trough since re-arm (squared) */
    uint32_t count, missed0, missed;
    uint64_t seen;                      /* samples processed since reset   */
    float peak2;                        /* squared, resonator units        */
} cn = { .f_hz = 50000u, .tau_us = 100u, .thr = 100u, .armed = true };

/* The resonator's coefficients and its gain for a steady tone at f, once
 * per configuration (from sigproc_app_block(), main loop). Analytic and exact:
 * for q[n] = Q e^(jwn) the magnitude |q1 - e^(-jw) q2| is |Q| |1 - e^(-2jw)|,
 * and for the negative frequency e^(-jwn) it is exactly 0 - the steady
 * magnitude has no ripple. A sine of amplitude 1 is half of each, so
 *     scale = 1/2 |1 - e^(-jw)| |H(e^(jw))| |1 - e^(-2jw)|
 *           = 2 sin(w/2) sin(w) / |1 - c e^(-jw) + D^2 e^(-2jw)|
 * (the first factor is the first difference). */
static void cnt_setup(void)
{
    cn.ready = false;
    const float fs = cn.fs;
    if (fs <= 0.0f) { return; }
    const float w = 6.283185307f * (float)cn.f_hz / fs;
    const float d = expf(-1.0e6f / ((float)cn.tau_us * fs));
    cn.cw = cosf(w);
    cn.sw = sinf(w);
    cn.c  = 2.0f * d * cn.cw;
    cn.dd = d * d;
    const float c2w = cosf(2.0f * w), s2w = sinf(2.0f * w);
    const float dre = 1.0f - cn.c * cn.cw + cn.dd * c2w;
    const float dim = cn.c * cn.sw - cn.dd * s2w;
    const float scale = 2.0f * sinf(0.5f * w) * cn.sw / sqrtf(dre * dre + dim * dim);
    const float t = (float)cn.thr * scale;
    cn.thr2 = t * t;
    cn.rearm2 = 0.25f * cn.thr2;                   /* (thr / 2)^2 */
    cn.inv_scale2 = 1.0f / (scale * scale);
    cn.q1 = cn.q2 = 0.0f;
    cn.ready = true;
}

static void cnt_block(const uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    bool fresh = false;
    if (cn.dirty) { cn.dirty = false; cnt_setup(); fresh = true; }
    if (cn.reset) {
        cn.reset = false;
        cn.count = 0u; cn.seen = 0u; cn.peak2 = 0.0f;
        cn.missed0 = info->missed;
        /* not armed, with what rings right now as the peak: a reset in
         * the middle of a ring must not count that ring as a new one; it
         * re-arms once that has fallen (or at once when nothing rings) */
        const float re = cn.q1 - cn.q2 * cn.cw, im = cn.q2 * cn.sw;
        cn.armed = false;
        cn.pk2 = cn.ready ? (re * re + im * im) : 0.0f;
    }
    if (!cn.ready) { return; }
    /* After a new set-up or a gap the first difference starts at the
     * block's first sample: from a stale (or the initial 0) previous value
     * the step to ~2048 would kick the resonator like a ring of its own -
     * the relative re-arm then measured the real rings against that
     * phantom peak and never counted (host test, 02.10.2026). */
    if (fresh || info->gap) { cn.q1 = cn.q2 = 0.0f; cn.xp = (float)x[0]; }
    cn.missed = info->missed - cn.missed0;
    float q1 = cn.q1, q2 = cn.q2, xp = cn.xp, pk = cn.peak2;
    const float c = cn.c, dd = cn.dd, cw = cn.cw, sw = cn.sw;
    const float thr2 = cn.thr2, rearm2 = cn.rearm2;
    bool armed = cn.armed;
    float pkc = cn.pk2, tr = cn.tr2;
    uint32_t count = cn.count, i = 0u;
    for (; i + CNT_EVERY <= n; i += CNT_EVERY) {
        for (uint32_t k = 0; k < CNT_EVERY; k++) {
            const float u = (float)x[i + k];
            const float q0 = (u - xp) + c * q1 - dd * q2;
            xp = u; q2 = q1; q1 = q0;
        }
        const float re = q1 - q2 * cw, im = q2 * sw;
        const float m2 = re * re + im * im;
        if (m2 > pk) { pk = m2; }
        if (armed) {
            if (m2 < tr) { tr = m2; }
            if ((m2 > thr2) && (m2 * CNT_DROP2 > tr)) { count++; armed = false; pkc = m2; }
        } else {
            if (m2 > pkc) { pkc = m2; }
            if ((m2 < CNT_DROP2 * pkc) || (m2 < rearm2)) { armed = true; tr = m2; }
        }
    }
    for (; i < n; i++) {                              /* n mod 4: no evaluation */
        const float u = (float)x[i];
        const float q0 = (u - xp) + c * q1 - dd * q2;
        xp = u; q2 = q1; q1 = q0;
    }
    cn.q1 = q1; cn.q2 = q2; cn.xp = xp; cn.peak2 = pk;
    cn.armed = armed; cn.count = count; cn.pk2 = pkc; cn.tr2 = tr;
    cn.seen += n;
}

/* ---- sigproc.h's application hooks ---- */

bool sigproc_app_active(void) { return cn.on; }

void sigproc_app_set_fs(float fs_hz)
{
    if (fs_hz != cn.fs) { cn.fs = fs_hz; cn.dirty = true; }
}

void sigproc_app_block(const uint16_t *x, uint32_t n, const sigproc_info_t *info)
{
    if (cn.on) { cnt_block(x, n, info); }
}

/* ---- the counter's own interface (impact.h) ---- */

void impact_enable(bool on)
{
    if (on && !cn.on) { cn.dirty = true; cn.reset = true; }
    cn.on = on;
}

bool impact_on(void) { return cn.on; }

bool impact_config(uint32_t f_hz, uint32_t tau_us, uint32_t thr)
{
    if ((f_hz < 1000u) || (tau_us < 2u) || (tau_us > 5000u) || (thr < 1u) || (thr > 4095u)) {
        return false;
    }
    if ((cn.fs > 0.0f) && ((float)f_hz * 2.5f > cn.fs)) { return false; }
    cn.f_hz = f_hz; cn.tau_us = tau_us; cn.thr = thr;
    cn.dirty = true;
    return true;
}

void impact_reset(void) { cn.reset = true; }

void impact_get(impact_t *out, bool clear_peak)
{
    out->on = cn.on ? 1u : 0u;
    out->f_hz = cn.f_hz;
    out->tau_us = cn.tau_us;
    out->thr = cn.thr;
    out->fs_hz = (uint32_t)(cn.fs + 0.5f);
    const bool fresh = cn.reset;                  /* a reset not yet applied */
    out->count = fresh ? 0u : cn.count;
    out->missed = fresh ? 0u : cn.missed;
    const float secs = (cn.fs > 0.0f && !fresh) ? (float)cn.seen / cn.fs : 0.0f;
    out->ms = (uint32_t)(secs * 1000.0f + 0.5f);
    out->rate = (secs > 0.0f) ? (uint32_t)((float)out->count / secs + 0.5f) : 0u;
    out->peak = (cn.ready && !fresh) ? (uint32_t)(sqrtf(cn.peak2 * cn.inv_scale2) + 0.5f) : 0u;
    if (clear_peak) { cn.peak2 = 0.0f; }
}

/* ---- the console: "sigproc cnt ..." (console.h's application hooks) ----
 *   sigproc cnt on | off | reset                 the counter
 *   sigproc cnt f <hz> | tau <us> | thr <lsb>    its resonator and threshold
 * cli.c's cmd_sigproc_fn() calls this for every sub-command it does not
 * know, and switches the processing on or off after it (sigproc_active()). */
int sigproc_app_cmd(int argc, char **argv)
{
    static const char use[] = "sigproc cnt on|off|reset | cnt f <hz> | cnt tau <us> | cnt thr <lsb>";
    if ((argc < 2) || (strcmp(argv[1], "cnt") != 0)) { return 0; }
    if (argc == 3) {
        if (strcmp(argv[2], "on") == 0)         { impact_enable(true); }
        else if (strcmp(argv[2], "off") == 0)   { impact_enable(false); }
        else if (strcmp(argv[2], "reset") == 0) { impact_reset(); }
        else { usage(use); return -1; }
        return 1;
    }
    if (argc == 4) {
        impact_t cur;
        impact_get(&cur, false);
        uint32_t v, f = cur.f_hz, tau = cur.tau_us, thr = cur.thr;
        if (!arg_u32(argv[3], 1u, 1000000u, &v)) { usage(use); return -1; }
        if (strcmp(argv[2], "f") == 0)        { f = v; }
        else if (strcmp(argv[2], "tau") == 0) { tau = v; }
        else if (strcmp(argv[2], "thr") == 0) { thr = v; }
        else { usage(use); return -1; }
        if (!impact_config(f, tau, thr)) {
            usage("cnt f 1000..fs/2.5 Hz, tau 2..5000 us, thr 1..4095 LSB");
            return -1;
        }
        return 1;
    }
    usage(use);
    return -1;
}

/* The counter's lines in "sigproc"'s status, after the Goertzel's. */
void sigproc_app_status(void)
{
    impact_t cn_;
    impact_get(&cn_, false);
    put_kv_str("cnt", cn_.on ? "on" : "off");
    put_kv("cnt_f_hz", cn_.f_hz);
    put_kv("cnt_tau_us", cn_.tau_us);
    put_kv("cnt_thr", cn_.thr);
    if (cn_.on) {
        put_kv("cnt_count", cn_.count);
        put_kv("cnt_rate", cn_.rate);
        put_kv("cnt_ms", cn_.ms);
        put_kv("cnt_missed", cn_.missed);
        put_kv("cnt_peak", cn_.peak);
        put_kv("cnt_fs_hz", cn_.fs_hz);
    }
}

/* ---- the GRAB header: " cnt= cnr= cpk=" while the counter runs - the
 * count and rate since its reset, and the largest magnitude since the
 * previous grab. At most 3 x 5 + 3 x 10 = 45 characters. */
char *gui_link_app_fields(char *p, const char *end)
{
    if (!cn.on || (end - p) < 45) { return p; }
    impact_t c;
    impact_get(&c, true);
    p = copy_str(p, " cnt="); p = u32_to_str(p, c.count);
    p = copy_str(p, " cnr="); p = u32_to_str(p, c.rate);
    p = copy_str(p, " cpk="); p = u32_to_str(p, c.peak);
    return p;
}

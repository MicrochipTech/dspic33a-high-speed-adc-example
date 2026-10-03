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
 * tri_eval.c - the chain test's triangle evaluator, hardware-free
 *
 * Moved verbatim out of chaintest.c on 27.09.2026 (P2.3): the comment
 * block, fit_line(), tri_eval(), the grid verdict tri_grid_ok() and the
 * scratch arrays they share. Only `static` was dropped from the three
 * functions; TP_MAX, STEP_CHECK_LSB, GRID_SLIP_MAX and tri_t are in
 * tri_eval.h. Nothing here touches a register or the DMA - the caller
 * hands over the samples and their count.
 */
#include <stdint.h>
#include <stdbool.h>
#include "tri_eval.h"

/* ------------------------------------------------------------------ *
 * The triangle evaluator
 *
 * 1. Coarse turning points with a hysteresis of a quarter of the swing.
 * 2. A straight line fitted to the inside of every segment between them
 *    (an eighth of the segment left out at each end: the first DAC step
 *    after a turn uses a scaled SLPDAT, p1421, and the ADC's settling
 *    rounds the corner).
 * 3. Refined turning points where neighbouring lines intersect - to a
 *    fraction of a sample, independent of DNL.
 * 4. Complete slopes = distance between refined turning points, reported
 *    per direction with their largest deviation (dev).
 * 5. THE GRID VERDICT, "slip": a lost sample shifts every later turning
 *    point by exactly one sample, a repeated one by minus one. The
 *    turning points at the ends of the slope the fault sits in move by
 *    about half of that each (the line through that slope is a
 *    compromise), so a single slope only comes out half a sample off -
 *    too close to the noise at short slopes. Four turning points that
 *    enclose the faulty slope carry the whole sample: (P[j+4] - P[j]) -
 *    2 * T, T the median period, is 0 on a clean grid and +-1 across a
 *    fault. With fewer than five turning points it falls back to one
 *    period, (P[j+2] - P[j]) - T, which sees only about half a fault.
 * 6. Where the step per sample is at least 40 LSB, every single step
 *    inside a segment: below half a step is a repeated sample, above one
 *    and a half steps a lost one. Not below 40: the DAC's DNL of +-5 LSB
 *    (Table 40-42) makes the difference of two neighbouring samples
 *    scatter by +-10 LSB plus noise, and at 14 LSB per sample a clean
 *    ramp already produced "repeated" and "lost" steps in the host test
 *    of this code (25.09.2026).
 *
 * The end segments are cut to the median slope length before their line
 * is fitted: the window can end shortly after a turning point the
 * hysteresis has not recognised yet, and a line through both sides of it
 * put the last real turning point 1.7 samples off in that host test.
 * ------------------------------------------------------------------ */

static uint16_t tp_idx[TP_MAX];
static double   per_tmp[TP_MAX];
static double   seg_a[TP_MAX + 1u], seg_b[TP_MAX + 1u];
static bool     seg_ok[TP_MAX + 1u];
static double   tp_pos[TP_MAX];

bool fit_line(const volatile uint16_t *x, uint32_t a, uint32_t b,
                     double *slope, double *icpt)
{
    if ((b <= a) || ((b - a) < 5u)) { return false; }
    const double c = 0.5 * (double)(a + b);
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double n = (double)(b - a + 1u);
    for (uint32_t i = a; i <= b; i++) {
        const double u = (double)i - c;
        const double y = (double)x[i];
        sx += u; sy += y; sxx += u * u; sxy += u * y;
    }
    const double den = n * sxx - sx * sx;
    if (den == 0.0) { return false; }
    const double m = (n * sxy - sx * sy) / den;
    *slope = m;
    *icpt  = (sy - m * sx) / n - m * c;      /* y = m * i + icpt           */
    return true;
}

void tri_eval(const volatile uint16_t *x, uint32_t n, tri_t *r)
{
    uint32_t mn = 0xFFFFu, mx = 0u;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] < mn) { mn = x[i]; }
        if (x[i] > mx) { mx = x[i]; }
    }
    r->mn = mn; r->mx = mx; r->pp = mx - mn;
    r->tps = 0u; r->n_up = 0u; r->n_dn = 0u;
    r->l_up = 0.0f; r->l_dn = 0.0f; r->dev = 0.0f; r->step = 0.0f;
    r->zero = 0u; r->dbl = 0u; r->step_checked = false; r->overflow = false;
    r->slip = 99.0f; r->slip_k = 0u; r->slip_n = 0u;
    if (n < 16u) { return; }

    /* 1. coarse turning points */
    uint32_t h = r->pp / 4u;
    if (h < 8u) { h = 8u; }
    int dir = 0;
    uint32_t hi_v = x[0], lo_v = x[0], hi_i = 0u, lo_i = 0u;
    uint32_t ext_v = x[0], ext_i = 0u;
    uint32_t ntp = 0u;
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = x[i];
        if (dir == 0) {
            if (v > hi_v) { hi_v = v; hi_i = i; }
            if (v < lo_v) { lo_v = v; lo_i = i; }
            if (v + h < hi_v) {                      /* now falling        */
                if ((hi_i > 2u) && (ntp < TP_MAX)) { tp_idx[ntp++] = (uint16_t)hi_i; }
                dir = -1; ext_v = v; ext_i = i;
            } else if (v > lo_v + h) {               /* now rising         */
                if ((lo_i > 2u) && (ntp < TP_MAX)) { tp_idx[ntp++] = (uint16_t)lo_i; }
                dir = 1; ext_v = v; ext_i = i;
            }
        } else if (dir > 0) {
            if (v >= ext_v) { ext_v = v; ext_i = i; }
            else if ((ext_v - v) > h) {
                if (ntp < TP_MAX) { tp_idx[ntp++] = (uint16_t)ext_i; } else { r->overflow = true; }
                dir = -1; ext_v = v; ext_i = i;
            }
        } else {
            if (v <= ext_v) { ext_v = v; ext_i = i; }
            else if ((v - ext_v) > h) {
                if (ntp < TP_MAX) { tp_idx[ntp++] = (uint16_t)ext_i; } else { r->overflow = true; }
                dir = 1; ext_v = v; ext_i = i;
            }
        }
    }
    r->tps = ntp;
    if (ntp < 2u) { return; }

    /* the median distance between coarse turning points, for the ends */
    uint32_t med = n;
    {
        uint32_t cnt = 0u;
        for (uint32_t j = 1; j < ntp; j++) { per_tmp[cnt++] = (double)(tp_idx[j] - tp_idx[j - 1u]); }
        for (uint32_t i = 1; i < cnt; i++) {                  /* insertion sort */
            const double v = per_tmp[i];
            uint32_t k = i;
            while ((k > 0u) && (per_tmp[k - 1u] > v)) { per_tmp[k] = per_tmp[k - 1u]; k--; }
            per_tmp[k] = v;
        }
        if (cnt != 0u) { med = (uint32_t)per_tmp[cnt / 2u]; }
    }

    /* 2. a line through the inside of every segment */
    double step_sum = 0.0;
    uint32_t step_n = 0u;
    for (uint32_t s = 0; s <= ntp; s++) {
        uint32_t a = (s == 0u) ? 0u : tp_idx[s - 1u];
        uint32_t b = (s == ntp) ? (n - 1u) : tp_idx[s];
        if ((s == 0u) && (b > med))           { a = b - med; }  /* cut the ends */
        if ((s == ntp) && ((b - a) > med))    { b = a + med; }
        const uint32_t len = b - a;
        uint32_t m = len / 8u;
        if (m < 2u) { m = 2u; }
        seg_ok[s] = (len > 2u * m + 5u) && fit_line(x, a + m, b - m, &seg_a[s], &seg_b[s]);
        if (seg_ok[s] && (s > 0u) && (s < ntp)) {      /* inner segments   */
            step_sum += (seg_a[s] >= 0.0) ? seg_a[s] : -seg_a[s];
            step_n++;
        }
    }
    r->step = (step_n != 0u) ? (float)(step_sum / (double)step_n) : 0.0f;

    /* 5. every step inside the inner segments, where it is large enough */
    if (r->step >= STEP_CHECK_LSB) {
        r->step_checked = true;
        for (uint32_t s = 1; s < ntp; s++) {
            if (!seg_ok[s]) { continue; }
            const uint32_t a = tp_idx[s - 1u], b = tp_idx[s];
            uint32_t m = (b - a) / 8u;
            if (m < 2u) { m = 2u; }
            const double sl = seg_a[s];
            const double as = (sl >= 0.0) ? sl : -sl;
            for (uint32_t i = a + m; i < b - m; i++) {
                const double d = ((double)x[i + 1u] - (double)x[i]) * ((sl >= 0.0) ? 1.0 : -1.0);
                if (d < 0.5 * as)      { r->zero++; }
                else if (d > 1.5 * as) { r->dbl++; }
            }
        }
    }

    /* 3. refined turning points: line s meets line s+1 */
    for (uint32_t j = 0; j < ntp; j++) {
        if (seg_ok[j] && seg_ok[j + 1u] && (seg_a[j] != seg_a[j + 1u])) {
            tp_pos[j] = (seg_b[j + 1u] - seg_b[j]) / (seg_a[j] - seg_a[j + 1u]);
        } else {
            tp_pos[j] = -1.0;
        }
    }

    /* 4. complete slopes: inner segments between two refined points */
    double up = 0.0, dn = 0.0;
    for (uint32_t s = 1; s < ntp; s++) {
        if ((tp_pos[s - 1u] < 0.0) || (tp_pos[s] < 0.0) || !seg_ok[s]) { continue; }
        const double L = tp_pos[s] - tp_pos[s - 1u];
        if (seg_a[s] > 0.0) { up += L; r->n_up++; } else { dn += L; r->n_dn++; }
    }
    if (r->n_up != 0u) { r->l_up = (float)(up / (double)r->n_up); }
    if (r->n_dn != 0u) { r->l_dn = (float)(dn / (double)r->n_dn); }
    double dev = 0.0;
    for (uint32_t s = 1; s < ntp; s++) {
        if ((tp_pos[s - 1u] < 0.0) || (tp_pos[s] < 0.0) || !seg_ok[s]) { continue; }
        const double L = tp_pos[s] - tp_pos[s - 1u];
        const double d = L - ((seg_a[s] > 0.0) ? (double)r->l_up : (double)r->l_dn);
        const double ad = (d >= 0.0) ? d : -d;
        if (ad > dev) { dev = ad; }
    }
    r->dev = (float)dev;

    /* 5. slip over two periods (or one, with few turning points). Only
     * turning points between two FULL slopes count, i.e. not the first
     * and the last: next to them lies a segment the window cuts, whose
     * line sees one rounded corner instead of two and is biased
     * differently - 0.7 samples at 40 MSPS with the DAC filter modelled,
     * a false "slip" on clean data (host test, 25.09.2026). */
    const uint32_t first = 1u, last = ntp - 2u;           /* inclusive     */
    const uint32_t inner = (ntp >= 3u) ? (last - first + 1u) : 0u;
    const uint32_t k = (inner >= 5u) ? 4u : 2u;
    uint32_t cnt = 0u;
    for (uint32_t j = first; (inner != 0u) && ((j + 2u) <= last); j++) {   /* periods -> median T */
        if ((tp_pos[j] >= 0.0) && (tp_pos[j + 2u] >= 0.0)) { per_tmp[cnt++] = tp_pos[j + 2u] - tp_pos[j]; }
    }
    if (cnt == 0u) { return; }
    for (uint32_t i = 1; i < cnt; i++) {
        const double v = per_tmp[i];
        uint32_t q = i;
        while ((q > 0u) && (per_tmp[q - 1u] > v)) { per_tmp[q] = per_tmp[q - 1u]; q--; }
        per_tmp[q] = v;
    }
    const double T = per_tmp[cnt / 2u];
    double worst = 0.0;
    uint32_t spans = 0u;
    for (uint32_t j = first; (j + k) <= last; j++) {
        if ((tp_pos[j] < 0.0) || (tp_pos[j + k] < 0.0)) { continue; }
        const double e = (tp_pos[j + k] - tp_pos[j]) - (double)(k / 2u) * T;
        const double ae = (e >= 0.0) ? e : -e;
        if (ae > worst) { worst = ae; }
        spans++;
    }
    if (spans != 0u) { r->slip = (float)worst; r->slip_k = k; r->slip_n = spans; }
}

/* The grid verdict: slopes of both directions seen, no turning point
 * half a sample or more off the grid over two periods, no repeated or
 * lost step where steps are checked. */
bool tri_grid_ok(const tri_t *r)
{
    return (r->n_up >= 1u) && (r->n_dn >= 1u) && !r->overflow && (r->slip_n != 0u) &&
           (r->slip < GRID_SLIP_MAX) && (r->zero == 0u) && (r->dbl == 0u);
}

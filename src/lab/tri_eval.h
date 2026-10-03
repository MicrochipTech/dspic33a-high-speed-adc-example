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
 * tri_eval.h - the chain test's triangle evaluator, hardware-free
 *
 * Moved verbatim out of chaintest.c on 27.09.2026 (P2.3, docs/
 * IMPLEMENTATION-PLAN.md) so that it can be built and tested on the host
 * (tests/host/test_tri_eval.c) and cross-checked against its Python port
 * in tools/eval_chain.py (tests/host/test_tri_eval_xcheck.py). The
 * algorithm and the reasons behind its thresholds are in tri_eval.c.
 *
 * The sample pointer keeps its `volatile`: the chain test evaluates the
 * DMA buffer in place (capture_buffer()), and the evaluator was moved as
 * it was, not re-typed. A plain `const uint16_t *` converts to it
 * implicitly, so the host test passes ordinary arrays.
 */
#ifndef TRI_EVAL_H
#define TRI_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#define TP_MAX          160u
#define STEP_CHECK_LSB  40.0

typedef struct {
    uint32_t mn, mx, pp;
    uint32_t tps;                 /* coarse turning points found         */
    uint32_t n_up, n_dn;          /* complete rising / falling slopes    */
    float    l_up, l_dn;          /* their mean length in samples        */
    float    dev;                 /* largest deviation from the mean     */
    float    step;                /* mean |slope| in LSB per sample      */
    uint32_t zero, dbl;           /* repeated / lost sample steps        */
    float    slip;                /* the grid verdict's number, samples  */
    uint32_t slip_k;              /* 4 (two periods) or 2 (one period)   */
    uint32_t slip_n;              /* how many spans were compared        */
    bool     step_checked;
    bool     overflow;            /* more turning points than TP_MAX     */
} tri_t;

#define GRID_SLIP_MAX   0.5f

/* A straight line y = slope * i + icpt through x[a..b] (inclusive), by
 * least squares about the centre of the span. false if the span is
 * shorter than 5 samples or degenerate. */
bool fit_line(const volatile uint16_t *x, uint32_t a, uint32_t b,
              double *slope, double *icpt);

/* Evaluate one window of n samples: every field of *r is written. */
void tri_eval(const volatile uint16_t *x, uint32_t n, tri_t *r);

/* The grid verdict: slopes of both directions seen, no turning point
 * half a sample or more off the grid over two periods, no repeated or
 * lost step where steps are checked. */
bool tri_grid_ok(const tri_t *r);

#endif /* TRI_EVAL_H */

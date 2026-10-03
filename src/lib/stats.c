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
 * stats.c - min/max/mean of a block of samples (see stats.h)
 *
 * half_stats() was `static` in cli.c and read the completed half itself
 * (capture_completed_half()/capture_half_len()); half_mean() was `static`
 * in capture.c. Both moved here on 27.09.2026 (P2.2) with the buffer and
 * its length as parameters instead, the loop bodies unchanged.
 */

#include "stats.h"

/* min/max/mean/pp of the completed half - shared by "stats" and the
 * periodic [half] line. */
void half_stats(const uint16_t *b, uint32_t n,
                uint32_t *mn, uint32_t *mx, uint32_t *mean)
{
    uint32_t lo = 0xFFFFu, hi = 0u, acc = 0u;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t v = b[i];
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
        acc += v;
    }
    *mn = lo; *mx = hi; *mean = acc / n;
}

uint32_t half_mean(const uint16_t *b, uint32_t n)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += b[i];
    }
    return acc / n;
}

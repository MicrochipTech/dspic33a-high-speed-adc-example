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
 * wavegen.c - synthesis of a signal-generator table (see wavegen.h)
 *
 * tab_wave_gen.py generate_wave() lines 35-63 in float32, two passes:
 * the first finds min and max of y, the second scales and rounds.
 */

#include <math.h>
#include <stddef.h>     /* NULL - not in math.h with xc-dsc */
#include "wavegen.h"

#define WAVEGEN_TWO_PI 6.283185307179586f

float wavegen_snap_hz(const wavegen_cfg_t *c)
{
    float periods = floorf(c->f0_hz * (float)c->n / (float)c->play_hz + 0.5f);
    if (periods < 1.0f) { periods = 1.0f; }
    return periods * (float)c->play_hz / (float)c->n;
}

/* y_i: script lines 51-58 (envelope, harmonic sum), amplitude left out
 * of the sum - it cancels in the normalisation. */
static float wavegen_y(const wavegen_cfg_t *c, float f0, uint32_t i)
{
    const float t = (float)i / (float)c->play_hz;
    const float w0t = WAVEGEN_TWO_PI * f0 * t;
    float s = sinf(w0t);
    for (uint32_t k = 0; k < 6u; k++) {
        if (c->harm[k] != 0.0f) {
            s += c->harm[k] * sinf((float)(k + 2u) * w0t);
        }
    }
    return expf(-c->decay * t) * s;
}

wavegen_result_t wavegen_fill(const wavegen_cfg_t *c, uint16_t *table,
                              bool snap, float *f0_used)
{
    if (c->n < 2u) { return WAVEGEN_E_N; }
    if (c->play_hz == 0u) { return WAVEGEN_E_RATE; }
    if (!(c->f0_hz > 0.0f) || !(c->f0_hz < 0.5f * (float)c->play_hz)) { return WAVEGEN_E_F0; }
    if (!(c->decay >= 0.0f)) { return WAVEGEN_E_DECAY; }
    if (!(c->amplitude > 0.0f) || !(c->amplitude <= 1.0f)) { return WAVEGEN_E_AMPLITUDE; }
    if (c->out_min >= c->out_max) { return WAVEGEN_E_RANGE; }

    const float f0 = snap ? wavegen_snap_hz(c) : c->f0_hz;

    /* pass 1: min and max (script lines 61-62) */
    float lo = wavegen_y(c, f0, 0u), hi = lo;
    for (uint32_t i = 1; i < c->n; i++) {
        const float y = wavegen_y(c, f0, i);
        if (y < lo) { lo = y; }
        if (y > hi) { hi = y; }
    }
    if (!(hi > lo)) { return WAVEGEN_E_FLAT; }

    /* pass 2: minimum -> out_min, maximum -> out_min + span x amplitude,
     * rounded to nearest (script line 63; half-way cases go up here,
     * to even in numpy - the host test allows the 1 LSB) */
    const float span = (float)(c->out_max - c->out_min) * c->amplitude;
    const float scale = span / (hi - lo);
    for (uint32_t i = 0; i < c->n; i++) {
        const float v = (wavegen_y(c, f0, i) - lo) * scale;
        table[i] = (uint16_t)((uint32_t)floorf(v + 0.5f) + c->out_min);
    }
    if (f0_used != NULL) { *f0_used = f0; }
    return WAVEGEN_OK;
}

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
 * iir1.c - first-order IIR low-pass / high-pass, one instance per filter
 *          (see iir1.h)
 *
 * The two step functions are the template's iFLT_IIR1_Lowpass() and
 * iFLT_IIR1_Highpass() (Goertzel/goertzel/firmware/src/goertzel.c, lines
 * 169-234) with the global tap array replaced by the instance and the
 * constant shift by f->k. Nothing else. The template compiles them with
 * __attribute__((optimize("-O1"))); the firmware builds at -O1 anyway.
 */

#include "iir1.h"

void iir1_init(iir1_t *f, uint8_t k)
{
    f->k = k;
    f->tap = 0;
}

void iir1_reset(iir1_t *f)
{
    f->tap = 0;
}

int32_t iir1_lp(iir1_t *f, int32_t x)
{
    int32_t tap = f->tap;
    tap = tap - (tap >> f->k) + x;
    f->tap = tap;
    return tap >> f->k;
}

int32_t iir1_hp(iir1_t *f, int32_t x)
{
    int32_t tap = f->tap;
    tap = tap - (tap >> f->k) + x;
    f->tap = tap;
    return x - (tap >> f->k);
}

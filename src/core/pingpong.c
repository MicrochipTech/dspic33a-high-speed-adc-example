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
 * pingpong.c - the two-half buffer's bookkeeping (see pingpong.h)
 *
 * pingpong_on_half(), the ISR-rate function, is `static inline` in the
 * header (see the design note there); everything below is called at most
 * once per completed half, from the main loop, so it stays an ordinary
 * function.
 */

#include "pingpong.h"

const volatile uint16_t *pingpong_completed_half(volatile uint16_t *data, uint32_t half_len,
                                                 uint32_t ready_half)
{
    return &data[ready_half ? half_len : 0u];
}

bool pingpong_guard_ok(const volatile uint32_t *guard, uint32_t guard_words,
                       uint32_t guard_base)
{
    for (uint32_t i = 0; i < guard_words; i++) {
        if (guard[i] != guard_base + i) { return false; }
    }
    return true;
}

bool pingpong_service(pingpong_t *pp, uint32_t blocks_done)
{
    if (blocks_done == pp->seen_blocks) {
        return false;
    }
    if ((blocks_done - pp->seen_blocks) > 1u) {
        pp->missed += (blocks_done - pp->seen_blocks) - 1u;
    }
    pp->seen_blocks = blocks_done;
    return true;
}

void pingpong_counters_clear(pingpong_t *pp, uint32_t blocks_done)
{
    pp->missed      = 0u;
    pp->seen_blocks = blocks_done;
}

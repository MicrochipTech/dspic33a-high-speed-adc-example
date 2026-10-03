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
 * ev17p63a.c - board_cfg for the EV17P63A (dsPIC33AK512MPS506 Curiosity
 * Nano). P7.1, docs/IMPLEMENTATION-PLAN.md; see src/boards/board_cfg.h for
 * what is and is not here, and why (most of board.h stayed a macro; this
 * is the one group that moved).
 */
#include "board_cfg.h"

/* The sample rate at boot, as PLL1's two output dividers - board-
 * independent today (both devices share the same PLL/ADC clock tree), so
 * the same value as src/boards/ev74h48a.c. See that file for the full
 * reasoning, moved from board.h's ADC_PLL_POSTDIV1/ADC_PLL_POSTDIV2. */
const board_cfg_t board_cfg = {
    .adc_pll_postdiv1 = 7u,
    .adc_pll_postdiv2 = 7u,
};

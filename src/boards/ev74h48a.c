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
 * ev74h48a.c - board_cfg for the EV74H48A (dsPIC33 Curiosity Platform
 * Development Board, dsPIC33AK512MPS512 GP DIM). P7.1,
 * docs/IMPLEMENTATION-PLAN.md; see src/boards/board_cfg.h for what is and
 * is not here, and why (most of board.h stayed a macro; this is the one
 * group that moved).
 */
#include "board_cfg.h"

/* The sample rate at boot, as PLL1's two output dividers: the ADC clock is
 * 1600 MHz / (POSTDIV1 * POSTDIV2), and eight of those clocks make one
 * back-to-back conversion. 7/7 = 32.65 MHz = 4.08 MSPS is the slowest
 * setting that still clears the ADC's 32 MHz minimum; 5/5 = 64 MHz =
 * 8 MSPS is what the customer's application needs; 5/1 = 320 MHz = 40 MSPS
 * is the maximum and what clock_init() starts with.
 *
 * The slowest setting is the default on purpose: it is the one the DMA
 * should manage comfortably, so the first test of a run is the one most
 * likely to pass, and a failure there means the chain itself is broken -
 * not the rate. "pll <p1> <p2>" changes it at run time.
 *
 * Why the PLL and not the CLKGEN6 divider: in runs 8 and 9 the divider
 * seemed to have no effect - but both runs measured under overrun load, an
 * instrument later found void, and every document names CLKGEN6 as the
 * ADC clock (ANALYSIS.md C.3, withdrawn 25.09.2026). The chain test
 * measures it at the generator itself (chaintest.c S8). The chain test
 * does not use this boot rate: it sets PLL1 to 5/1 and paces the ADC with
 * SCCP1. */
const board_cfg_t board_cfg = {
    .adc_pll_postdiv1 = 7u,
    .adc_pll_postdiv2 = 7u,
};

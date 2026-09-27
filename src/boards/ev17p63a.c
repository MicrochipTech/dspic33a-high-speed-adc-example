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

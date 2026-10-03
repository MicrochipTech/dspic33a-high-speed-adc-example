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
 * siggen.h - the signal generator: a wavegen table played by DMA channel 2
 * into a DAC, paced by SCCP2, with no CPU involvement while it plays
 * (siggen.c; docs/IMPLEMENTATION-PLAN.md section SG, requirement A2 of
 * docs/DESIGN-MULTICHANNEL.md).
 *
 * The parameters are those of tab_wave_gen.py (lib/wavegen.h has the
 * formula): f0, the 2nd..7th harmonic factors h2..h7, decay, amp, and the
 * output range lo..hi. They are set one at a time (siggen_set(), the
 * console's "siggen set <param> <value>" - a 64-character line does not
 * hold all of them at once) and take effect with the next siggen_start().
 *
 * No register and no device header here: the table goes through
 * lib/wavegen, the transport through dma.c (channel 2), the clock through
 * sccp.c (SCCP2), the output through dac.c, the resource claim through
 * routing.c. What a start does, in this order: check the parameters and
 * the claim (nothing touched on a refusal), fill the table, the DAC to a
 * static level at table[0], DMA channel 2 armed on the DAC's data half,
 * SCCP2 started - the first trigger then moves the first entry. Stop is
 * the reverse.
 */
#ifndef SIGGEN_H
#define SIGGEN_H

#include <stdbool.h>
#include <stdint.h>
#include "wavegen.h"

/* 8192 x 2 bytes = 16 KB, in the ".dma_buffer" section beside the ADC
 * buffer: RAM at P0.1 was 14.8 KB used of 64 KB and the stack starts at
 * 0x7B7C (xc-dsc -t, 29.09.2026), so ~50 KB of stack less 16 KB still
 * leaves far more than BR's 25 % unused. */
#define SIGGEN_N_MAX        8192u
#define SIGGEN_PLAY_HZ_MIN  100u
/* DAC settling to 1 %: 750 ns typical, 2000 ns maximum (Table 40-42,
 * DA07, p2036); no update-rate limit is given. 1 MHz is allowed as the
 * ceiling the board run's ladder goes up to - clean only below about
 * 500 kHz by the worst-case figure. */
#define SIGGEN_PLAY_HZ_MAX  1000000u
/* The DAC's usable range, 5..95 % of VDD (18.4.2 p1417, dac.h's
 * DAC_CODE_MIN/MAX): the default output range, and the limit for lo/hi
 * unless "force" is given (SG decision 4). */
#define SIGGEN_LO_DEFAULT   0x0CDu
#define SIGGEN_HI_DEFAULT   0xF32u

typedef enum {
    SIGGEN_OK = 0,
    SIGGEN_E_PARAM,      /* no such parameter                           */
    SIGGEN_E_VALUE,      /* value out of the parameter's range          */
    SIGGEN_E_DAC,        /* DAC not 1 or 2                              */
    SIGGEN_E_N,          /* n outside 2..SIGGEN_N_MAX                   */
    SIGGEN_E_RATE,       /* play_hz outside MIN..MAX                    */
    SIGGEN_E_RANGE,      /* lo/hi outside the DAC's range without force */
    SIGGEN_E_WAVEGEN,    /* wavegen_fill() refused - siggen_wavegen_err() */
    SIGGEN_E_ROUTE,      /* routing refused the claim - siggen_route_err() */
    SIGGEN_E_DAC_START,  /* dac_level_start() refused                   */
    SIGGEN_E_DMA,        /* dma_tx_start() refused                     */
    SIGGEN_E_CLOCK       /* sccp2_start() refused                       */
} siggen_result_t;

/* Set one parameter, by name, to `micro` millionths (fmt_parse_dec()'s
 * form): "f0" (Hz, > 0), "h2".."h7" (factor, -100..100), "decay" (1/s, 0..1e9,
 * >= 0), "amp" (0 < amp <= 1), "lo"/"hi" (DAC codes 0..4095, whole
 * numbers). Checked against these ranges here; the combination (f0 below
 * play_hz / 2, lo < hi, ...) at siggen_start(). */
siggen_result_t siggen_set(const char *name, int64_t micro);

/* Stop whatever runs, then play the table: `dac` 1 or 2, `n` entries at
 * `play_hz`, `snap` = the fundamental moved to a whole number of periods
 * in the table (wavegen.h), `force` = lo/hi outside 0x0CD..0xF32 allowed,
 * `pace` = how SCCP2 raises the DMA trigger (sccp.h, 0 = TMR16). On any
 * refusal nothing is left running. */
siggen_result_t siggen_start(uint8_t dac, uint32_t n, uint32_t play_hz,
                             bool snap, bool force, uint8_t pace);
void            siggen_stop(void);
/* Stop the generator if it plays on `dac` - for every path that is about
 * to start or stop that DAC for something else (cli.c's "dac", the DAC
 * test, the chain test's triangle). */
void            siggen_release_dac(uint8_t dac);

bool     siggen_running(void);
uint8_t  siggen_dac(void);            /* 0 when off                      */
uint32_t siggen_actual_hz(void);      /* SCCP2's clock / its period      */
float    siggen_f0_used(void);        /* after snap                      */
uint8_t  siggen_wavegen_err(void);    /* wavegen_result_t of the last try */
uint8_t  siggen_route_err(void);      /* route_err_t of the last try     */
const char *siggen_result_name(siggen_result_t r);

/* The status report, one field per call, print-free like routing_visit():
 *   SIGGEN_VIS_NUM  name = key, v = value (decimal)
 *   SIGGEN_VIS_DEC  name = key, v = a value in millionths (int64 as the
 *                   bit pattern of v; cli.c prints it with dec_to_str())
 *   SIGGEN_VIS_HEX  name = key, v = value (hex)
 * In this order: on, dac, n, play_hz (set), play_hz_actual, pace, f0, f0_used
 * (DEC), h2..h7, decay, amp (DEC), lo, hi, snap, force, table_min,
 * table_max, dma2_stat (HEX), dma2_on, transfers_per_s (measured over a
 * few ms while running, 0 otherwise), sccp2_flags (bit 0 CCT2IF, bit 1
 * CCP2IF rose in that window), window_gap. */
typedef enum { SIGGEN_VIS_NUM, SIGGEN_VIS_DEC, SIGGEN_VIS_HEX } siggen_vis_fmt_t;
typedef void (*siggen_visit_t)(const char *name, int64_t v, siggen_vis_fmt_t fmt);
void siggen_visit(siggen_visit_t visit);

/* The table as played, for a test or a status that wants it. */
const uint16_t *siggen_table(void);
uint32_t        siggen_n(void);

#endif /* SIGGEN_H */

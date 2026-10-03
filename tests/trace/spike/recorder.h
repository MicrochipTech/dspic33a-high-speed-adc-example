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

/* recorder.h - P0.3 spike: the register-trace recorder (recorder.c). */
#ifndef RECORDER_H
#define RECORDER_H

#include <stddef.h>
#include <stdint.h>

typedef void (*sfr_hook_t)(unsigned idx);

void     trace_init(void);                     /* zero all, arm mode 3 guard */
void     trace_flush(void);                    /* modes 1/2: diff -> W lines */
void     trace_note(const char *fmt, ...);     /* flush, then one text line */
unsigned trace_idx(const char *name);          /* SFR name -> index         */
void     trace_hook(unsigned idx, sfr_hook_t h);   /* read hook (modes 2/3) */
void     trace_region(const volatile void *p, size_t n, const char *name);
uint32_t hw_get(unsigned idx);                 /* harness access, not traced */
void     hw_set(unsigned idx, uint32_t v);     /* "hardware" changes a value */
unsigned long trace_accesses(void);
const char   *trace_value(uint32_t v, char *buf, size_t len); /* symbolic if an address */

#endif

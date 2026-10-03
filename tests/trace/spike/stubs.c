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

/* stubs.c - P0.3 spike: what dma.c and timebase.c call outside themselves.
 * Console output goes into the trace as C lines, with host addresses made
 * symbolic; fail() ends the scenario step through longjmp (on the target
 * it never returns either). */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include "recorder.h"
#include "console.h"
#include "diag.h"

jmp_buf fail_jmp;
volatile uint32_t fail_code;

void console_puts(const char *s)  { trace_note("C %s", s); }
void console_trace(const char *s) { trace_note("C %s", s); }

void console_kv_hex(const char *k, uint32_t v)
{
    char b[48];
    trace_note("C %s: %s\r\n", k, trace_value(v, b, sizeof b));
}

void console_trace_kv_hex(const char *k, uint32_t v)
{
    char b[48];
    trace_note("C %s: %s\r\n", k, trace_value(v, b, sizeof b));
}

void fail(uint32_t code)
{
    fail_code = code;
    trace_note("F fail(%lu)\n", (unsigned long)code);
    longjmp(fail_jmp, 1);
}

void dma0_event(uint32_t status)
{
    trace_note("D dma0_event(0x%08lX)\n", (unsigned long)status);
}

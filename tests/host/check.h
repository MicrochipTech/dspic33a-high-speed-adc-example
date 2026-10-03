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
 * check.h - minimal host-side assert header for tests/host/test_*.c
 *
 * No external framework: CHECK(cond) and CHECK_EQ(a, b) count and record
 * failures, check_summary() prints one summary line and returns the exit
 * code main() should return (0 = all checks passed, 1 = at least one
 * failed). Everything is `static`, so each test_*.c that includes this
 * header gets its own private counters - fine, since each test file is
 * its own translation unit with its own main().
 */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

static int check_total_ = 0;
static int check_failed_ = 0;

#define CHECK(cond) \
    do { \
        check_total_++; \
        if (!(cond)) { \
            check_failed_++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                    __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        check_total_++; \
        unsigned long a_ = (unsigned long)(a); \
        unsigned long b_ = (unsigned long)(b); \
        if (a_ != b_) { \
            check_failed_++; \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (=%lu) != %s (=%lu)\n", \
                    __FILE__, __LINE__, #a, a_, #b, b_); \
        } \
    } while (0)

/* Call once at the end of main(). Prints "<passed>/<total> checks passed"
 * and returns 0 if every check passed, 1 otherwise - so main can end with
 * `return check_summary();`. */
static int check_summary(void)
{
    int passed = check_total_ - check_failed_;
    printf("%d/%d checks passed\n", passed, check_total_);
    return check_failed_ == 0 ? 0 : 1;
}

#endif /* CHECK_H */

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
 * vectors.h - read the CSV files in tests/ref/vectors/ from a host test
 *             (P3.1 on)
 *
 * The vector files are written by the tests/ref/ scripts (goertzel_ref.py,
 * wavegen_ref.py): a first line
 * "# <generating command>", then a line of column names, then one row of
 * integers (or floats) per line. This header finds the vectors directory
 * and parses rows; like check.h it is `static`-only, no framework.
 *
 * Where the directory is: tools\hosttest.bat runs each test as
 * <root>\build\host\test_NAME.exe, so <root> is two directories above
 * argv[0]. That is tried first; if it does not open, the cwd-relative
 * "tests/ref/vectors/" (a test run by hand from the repo root) and its
 * "../" and "../../" forms (run from build/ or build/host/) follow.
 */
#ifndef VECTORS_H
#define VECTORS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The "# ..." header line of the file last opened, for tests that read a
 * parameter out of it (e.g. "# pulses=5"). */
static char vectors_header[512];

static FILE *vectors_try(const char *dir, const char *name)
{
    char path[1024];
    if (strlen(dir) + strlen(name) + 2 > sizeof path) { return NULL; }
    strcpy(path, dir);
    strcat(path, name);
    return fopen(path, "r");
}

/* Opens tests/ref/vectors/<name> and consumes its two header lines, so
 * the next vectors_row() returns the first data row. NULL if not found. */
static FILE *vectors_open(const char *argv0, const char *name)
{
    FILE *f = NULL;
    char dir[1024];

    /* 1. from argv[0]: strip "test_x.exe", "host", "build" */
    if (argv0 != NULL && strlen(argv0) < sizeof dir) {
        strcpy(dir, argv0);
        int stripped = 0;
        for (int i = 0; i < 3; i++) {
            char *s = strrchr(dir, '\\');
            char *t = strrchr(dir, '/');
            char *cut = (s > t) ? s : t;
            if (cut == NULL) { break; }
            *cut = '\0';
            stripped++;
        }
        if (stripped == 3) {
            strcat(dir, "/tests/ref/vectors/");
            f = vectors_try(dir, name);
        }
    }
    /* 2. relative to the cwd */
    if (f == NULL) { f = vectors_try("tests/ref/vectors/", name); }
    if (f == NULL) { f = vectors_try("../tests/ref/vectors/", name); }
    if (f == NULL) { f = vectors_try("../../tests/ref/vectors/", name); }
    if (f == NULL) {
        fprintf(stderr, "vectors_open: %s not found (argv0 = %s)\n",
                name, argv0 ? argv0 : "(null)");
        return NULL;
    }

    /* header: "# command", then the column names */
    vectors_header[0] = '\0';
    if (fgets(vectors_header, sizeof vectors_header, f) == NULL ||
        vectors_header[0] != '#') {
        fprintf(stderr, "vectors_open: %s has no '# command' line\n", name);
        fclose(f);
        return NULL;
    }
    char cols[512];
    if (fgets(cols, sizeof cols, f) == NULL) {
        fprintf(stderr, "vectors_open: %s has no column line\n", name);
        fclose(f);
        return NULL;
    }
    return f;
}

/* Reads the next row into v[0..n): 1 on a row, 0 at end of file, -1 if a
 * row has fewer than n comma-separated numbers (integers or floats;
 * everything is parsed with strtod, so an integer column read this way is
 * exact up to 2^53). */
static int vectors_row(FILE *f, double *v, int n)
{
    char line[1024];
    do {
        if (fgets(line, sizeof line, f) == NULL) { return 0; }
    } while (line[0] == '#' || line[0] == '\n' || line[0] == '\r');
    const char *p = line;
    for (int i = 0; i < n; i++) {
        char *end;
        v[i] = strtod(p, &end);
        if (end == p) { return -1; }
        p = end;
        if (*p == ',') { p++; }
    }
    return 1;
}

#endif /* VECTORS_H */

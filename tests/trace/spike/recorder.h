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

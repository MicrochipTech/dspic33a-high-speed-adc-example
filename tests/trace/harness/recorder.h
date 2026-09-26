/*
 * recorder.h - P0.4 register-trace recorder (recorder.c). Approach (a),
 * snapshot diff in plain C (tests/trace/README.md, decision of
 * 26.09.2026). One scenario = one process: trace_begin() at the start,
 * trace_point() at every point whose accumulated writes matter,
 * trace_end() at the end. Order between two trace_point() calls is not
 * recorded - only the net change since the previous one.
 */
#ifndef RECORDER_H
#define RECORDER_H

#include <stddef.h>
#include <stdint.h>

/* Zeroes every SFR (the reset value assumed throughout, README) and the
 * shadow copy, and prints a header comment naming the scenario. Call
 * once, first. */
void trace_begin(const char *scenario);

/* Diffs every SFR against the shadow copy, in address order, printing a
 * `W NAME old -> new` line for each one that changed since the last
 * trace_point()/trace_note() (or since trace_begin(), the first time) -
 * then a `# label` comment line. */
void trace_point(const char *label);

/* Same diff as trace_point(), then one caller-formatted line instead of
 * a plain comment - used by the stubs for `C`/`D`/`F` lines. */
void trace_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Final diff and a flush of stdout. Call once, last. */
void trace_end(void);

/* SFR name -> index into sfr_mem[], for a scenario's own setup (presetting
 * a register) and for hwmodel.c's rules. Exits the process if not found. */
unsigned trace_idx(const char *name);

/* Registers a RAM buffer so that trace_value() below prints addresses
 * into it symbolically (&name+offset) instead of a raw, non-deterministic
 * host pointer. */
void trace_region(const volatile void *p, size_t n, const char *name);

/* Reads/sets an SFR by index without going through the diff - used for
 * a scenario's own setup and by the hardware model (hwmodel.c) to answer
 * a polling loop. Thread-safe with trace_point()/trace_note()'s diff, so
 * the background model thread (hwmodel.c) can call these while the main
 * thread runs the driver under test.
 *
 * hw_set_masked() only touches `mask`'s bits of both sfr_mem[] and
 * shadow[] - not the whole word. This matters when the same register
 * also carries driver-written bits that trace_flush() has not diffed
 * yet (e.g. clock.c writes PLL1CON = 0x8100, then sets and waits on its
 * own PLLSWEN bit): a full-word hw_set() would copy the driver's
 * undiffed bits into shadow[] too and the write would silently vanish
 * from the trace. hw_set(idx, v) is hw_set_masked(idx, ~0u, v) - only
 * correct for a register no driver ever writes directly (TMR1 here). */
uint32_t hw_get(unsigned idx);
void     hw_set(unsigned idx, uint32_t v);
void     hw_set_masked(unsigned idx, uint32_t mask, uint32_t value);

/* Formats v: "&NAME(0xADDR)" if v is the host address of an SFR,
 * "&region+0xOFS" if it falls inside a trace_region(), else "0xVALUE". */
const char *trace_value(uint32_t v, char *buf, size_t len);

#endif /* RECORDER_H */

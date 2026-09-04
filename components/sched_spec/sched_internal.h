/*
 * sched_internal.h — component-private helpers shared between the YAML
 * lexer (sched_yaml.c) and the compiler (sched_compile.c). NOT installed in
 * include/: nothing here is public API.
 *
 * Both numeric parsers are tri-state so the two duration paths can never
 * diverge (re-review 2-1): 1 = parsed, 0 = not this syntax, -1 = numeric
 * shape but out of int64 range. All magnitude checks run BEFORE the
 * multiply that would overflow.
 */

#ifndef SCHED_INTERNAL_H
#define SCHED_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Decimal magnitude of exactly `len` digits (no sign). The magnitude is
 * checked against INT64_MAX, so INT64_MIN is deliberately not expressible —
 * same verdict as the plain-scalar lexer. */
int sched_u64_digits(const char *s, size_t len, int64_t *out);

/* Unsigned duration: digits + (ms|s|m|h) → milliseconds. "ms" is checked
 * before "m". -1 when the digits overflow int64 or the milliseconds do not
 * fit (checked as v > INT64_MAX / mult before the unit multiply). */
int sched_duration_ms(const char *s, size_t len, int64_t *out_ms);

#endif /* SCHED_INTERNAL_H */

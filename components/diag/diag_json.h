#pragma once
/*
 * diag_json.h — private bounded JSON writer shared by the crash summary and
 * the ambyte.log/1 record renderer. Pure C, no allocation.
 *
 * Log messages are arbitrary bytes (a formatted ESP_LOGE can carry anything a
 * caller printed, including an em dash cut in half by the 160-byte limit), and
 * a consumer that cannot parse one record would lose the whole batch. So the
 * string writer guarantees valid JSON AND valid UTF-8: control bytes become
 * \u00XX, and any byte that is not part of a well-formed UTF-8 sequence
 * becomes U+FFFD. Writes past `cap` latch `ok = false` instead of truncating;
 * callers check `ok` (and may roll back to a saved `len`).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
} diag_jw_t;

void diag_jw_init(diag_jw_t *w, char *buf, size_t cap);
void diag_jw_raw(diag_jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Quoted, escaped string; at most `max` source bytes are consumed. */
void diag_jw_strn(diag_jw_t *w, const char *s, size_t max);
static inline void diag_jw_str(diag_jw_t *w, const char *s) { diag_jw_strn(w, s, (size_t)-1); }
/* Roll back to an earlier length (re-arms ok when that length fits). */
void diag_jw_rewind(diag_jw_t *w, size_t len);

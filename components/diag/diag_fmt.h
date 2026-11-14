#pragma once
/*
 * diag_fmt.h — bounded printf-style formatter that NEVER allocates, for the
 * error-log capture hook only (review finding, 2026-11-14).
 *
 * WHY: the hook runs on any task, including the esp-mqtt task and holders of
 * event_log's s_mtx, so it must not enter the heap. newlib's full
 * vsnprintf (CONFIG_LIBC_NEWLIB_NANO_FORMAT is off in this tree) goes
 * _svfprintf_r → _dtoa_r → _Balloc → malloc for every %f/%e/%g, so it may not
 * be called from the hook. The downstream sinks (sd_logger's hook, the IDF
 * console) still use newlib on the logging task exactly as before this
 * feature; that pre-existing behaviour is untouched and out of scope here.
 *
 * Supported: flags - + space # 0, width and precision (digits or *), length
 * hh h l ll j z t L, conversions d i u o x X c s p % and f F e E g G
 * (rendered with integer arithmetic on the double: exact for the integer
 * part below 2^64 and correct to the printed precision; exact binary ties
 * round half-up where newlib rounds half-even, so "%.2f" of 85.125 is 85.13
 * here; |x| ≥ 2^64 under %f is printed in e-form). Fractional digits beyond
 * 17 are zero padding.
 *
 * Not supported, counted in *unsupported and rendered as a visible marker:
 * %a/%A (marker "<%a>", the double is consumed), %n (never written; the
 * pointer is consumed), wide %lc/%ls (marker; the argument is consumed), and
 * any unknown conversion — after which formatting STOPS, because the size of
 * the remaining arguments can no longer be known.
 *
 * Pure C, no libc beyond <stdarg.h>/<stddef.h>/<stdint.h>: tests/diag_host
 * compares it against the host libc for every supported form.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format into out (always NUL-terminated when cap > 0, truncated at cap-1).
 * Returns the number of bytes written (excluding the NUL). *unsupported (may
 * be NULL) is incremented once per unsupported conversion. */
size_t diag_vsnformat(char *out, size_t cap, const char *fmt, va_list ap, uint32_t *unsupported);

#ifdef __cplusplus
}
#endif

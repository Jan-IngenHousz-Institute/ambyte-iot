/*
 * diag_fmt.c — allocation-free bounded formatter (contract: diag_fmt.h).
 *
 * Every va_arg happens in diag_vsnformat itself (never in a helper taking a
 * va_list), which keeps the va_list semantics identical on Xtensa (struct)
 * and x86-64 (array). Helpers only ever see already-fetched values and write
 * through a bounded cursor; nothing here calls libc.
 */
#include "diag_fmt.h"

#include <stdbool.h>

typedef struct {
    char  *out;
    size_t cap;
    size_t len;          /* bytes written (never exceeds cap - 1) */
} fw_t;

static void put(fw_t *w, char c)
{
    if (w->cap > 0 && w->len + 1 < w->cap) w->out[w->len++] = c;
}

/* Both loops stop as soon as the buffer is full: the work per conversion is
 * bounded by the output capacity, never by a caller-supplied width. */
static void put_n(fw_t *w, const char *s, size_t n)
{
    for (size_t i = 0; i < n && w->len + 1 < w->cap; i++) w->out[w->len++] = s[i];
}

static void put_rep(fw_t *w, char c, size_t n)
{
    for (size_t i = 0; i < n && w->len + 1 < w->cap; i++) w->out[w->len++] = c;
}

/* Widths and precisions beyond this can never be shown by the capture (its
 * whole line buffer is smaller), so they are clamped — and counted as
 * unsupported — instead of driving ~2^31 iterations of padding on the
 * logging task (review round 2, finding 2). */
#define FMT_FIELD_MAX 256

/* Width / precision as parsed: a non-negative value already clamped to
 * FMT_FIELD_MAX. Negation happens in a 64-bit type, so INT_MIN is not UB. */
static int clamp_field(long long v, uint32_t *unsupported)
{
    if (v > FMT_FIELD_MAX) {
        if (unsupported) (*unsupported)++;
        return FMT_FIELD_MAX;
    }
    return (int)v;
}

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16 };

/* One padded field: [spaces][prefix][zeros][body][spaces]. */
static void emit(fw_t *w, int flags, int width, bool zero_ok,
                 const char *prefix, size_t plen, size_t zeros, const char *body, size_t blen)
{
    size_t total = plen + zeros + blen;
    size_t pad = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;
    if (!(flags & F_LEFT) && (flags & F_ZERO) && zero_ok) {
        zeros += pad;
        pad = 0;
    }
    if (!(flags & F_LEFT)) put_rep(w, ' ', pad);
    put_n(w, prefix, plen);
    put_rep(w, '0', zeros);
    put_n(w, body, blen);
    if (flags & F_LEFT) put_rep(w, ' ', pad);
}

/* Digits of v in `base`, most significant first; returns the count. */
static size_t utoa(uint64_t v, unsigned base, bool upper, char *buf)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = dig[v % base];
        v /= base;
    } while (v != 0);
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static void emit_int(fw_t *w, int flags, int width, int prec, bool neg, uint64_t mag,
                     unsigned base, bool upper, bool is_signed, bool is_ptr)
{
    char body[24];
    size_t blen = 0;
    if (!(prec == 0 && mag == 0)) blen = utoa(mag, base, upper, body);   /* C: %.0d of 0 prints nothing */
    char prefix[3];
    size_t plen = 0;
    if (is_signed) {
        if (neg) prefix[plen++] = '-';
        else if (flags & F_PLUS) prefix[plen++] = '+';
        else if (flags & F_SPACE) prefix[plen++] = ' ';
    }
    if (is_ptr || ((flags & F_ALT) && base == 16 && mag != 0)) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }
    size_t zeros = (prec > 0 && (size_t)prec > blen) ? (size_t)prec - blen : 0;
    if ((flags & F_ALT) && base == 8 && zeros == 0 && (blen == 0 || body[0] != '0')) zeros = 1;
    emit(w, flags, width, prec < 0, prefix, plen, zeros, body, blen);
}

/* ── floating point without allocation ─────────────────────────────────── */

#define FMT_MAX_FRAC 17          /* digits beyond this are padding zeros */
#define FMT_U64_LIMIT 18446744073709551616.0   /* 2^64 */

/* Non-negative a < 2^64 rendered as "I[.F]" with `prec` fractional digits.
 * Returns the length; out needs 21 + 1 + prec bytes (prec ≤ FMT_MAX_FRAC
 * computed, the caller pads further zeros itself). */
static size_t fixed(double a, int prec, bool alt, char *out, int *carry_digits)
{
    int p = prec > FMT_MAX_FRAC ? FMT_MAX_FRAC : prec;
    uint64_t ip = (uint64_t)a;
    double frac = a - (double)ip;
    char fd[FMT_MAX_FRAC];
    for (int i = 0; i < p; i++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d > 9) d = 9;
        fd[i] = (char)('0' + d);
        frac -= d;
    }
    if (frac * 10.0 >= 5.0) {                     /* round half up on the remainder */
        int i = p - 1;
        for (; i >= 0; i--) {
            if (fd[i] == '9') { fd[i] = '0'; continue; }
            fd[i]++;
            break;
        }
        if (i < 0) ip++;                          /* carried into the integer part */
    }
    size_t n = utoa(ip, 10, false, out);
    if (carry_digits) *carry_digits = (int)n;
    if (prec > 0 || alt) out[n++] = '.';
    for (int i = 0; i < p; i++) out[n++] = fd[i];
    return n;
}

/* Normalise a > 0 to m in [1, 10) and a decimal exponent, by repeated
 * scaling (≤ ~330 steps: no libm, no table, no allocation). */
static double normalise(double a, int *exp10)
{
    int e = 0;
    while (a >= 10.0) { a /= 10.0; e++; }
    while (a < 1.0) { a *= 10.0; e--; }
    *exp10 = e;
    return a;
}

/* Mantissa "d.ddd" + "e±XX" for a ≥ 0. */
static size_t sci(double a, int prec, bool alt, bool upper, char *out, int *exp_out)
{
    int e = 0;
    double m = a == 0.0 ? 0.0 : normalise(a, &e);
    int idigits = 0;
    size_t n = fixed(m, prec, alt, out, &idigits);
    if (idigits > 1) {                            /* 9.99… rounded up to 10.0… */
        m = m / 10.0;
        e++;
        n = fixed(m, prec, alt, out, &idigits);
    }
    if (exp_out) *exp_out = e;
    out[n++] = upper ? 'E' : 'e';
    out[n++] = e < 0 ? '-' : '+';
    char ed[8];
    size_t el = utoa((uint64_t)(e < 0 ? -e : e), 10, false, ed);
    if (el < 2) out[n++] = '0';
    for (size_t i = 0; i < el; i++) out[n++] = ed[i];
    return n;
}

/* Drop trailing fractional zeros (and a bare '.') from body[0..n), keeping
 * any exponent suffix that starts at `exp_at` (== n when there is none). */
static size_t strip_zeros(char *body, size_t n, size_t exp_at)
{
    size_t dot = n;
    for (size_t i = 0; i < exp_at; i++) if (body[i] == '.') { dot = i; break; }
    if (dot == n) return n;
    size_t end = exp_at;
    while (end > dot + 1 && body[end - 1] == '0') end--;
    if (end == dot + 1) end = dot;
    size_t k = end;
    for (size_t i = exp_at; i < n; i++) body[k++] = body[i];
    return k;
}

static void emit_double(fw_t *w, int flags, int width, int prec, char conv, double v)
{
    const bool upper = conv == 'F' || conv == 'E' || conv == 'G';
    union { double d; uint64_t u; } bits = {.d = v};
    const bool neg = (bits.u >> 63) != 0;
    char prefix[1];
    size_t plen = 0;
    if (neg) prefix[plen++] = '-';
    else if (flags & F_PLUS) prefix[plen++] = '+';
    else if (flags & F_SPACE) prefix[plen++] = ' ';
    double a = neg ? -v : v;
    if (v != v || (a - a) != (a - a)) {           /* NaN / infinity: no zero padding */
        const char *s = v != v ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit(w, flags, width, false, prefix, plen, 0, s, 3);
        return;
    }
    if (prec < 0) prec = 6;
    const bool alt = (flags & F_ALT) != 0;
    char body[64];
    size_t n = 0;
    size_t pad_zeros_after = 0;                   /* precision beyond FMT_MAX_FRAC */
    char c = conv | 0x20;                         /* lower-case conversion letter */
    if (c == 'f' && a >= FMT_U64_LIMIT) c = 'e';  /* too wide for the integer path */
    if (c == 'f') {
        n = fixed(a, prec, alt, body, NULL);
        if (prec > FMT_MAX_FRAC) pad_zeros_after = (size_t)(prec - FMT_MAX_FRAC);
    } else if (c == 'e') {
        n = sci(a, prec > FMT_MAX_FRAC ? FMT_MAX_FRAC : prec, alt, upper, body, NULL);
    } else {                                      /* g */
        int P = prec == 0 ? 1 : prec;
        if (P > FMT_MAX_FRAC + 1) P = FMT_MAX_FRAC + 1;
        int x = 0;
        char probe[64];
        (void)sci(a, P - 1, alt, upper, probe, &x);
        if (a == 0.0) x = 0;
        if (P > x && x >= -4 && a < FMT_U64_LIMIT) {
            n = fixed(a, P - 1 - x, alt, body, NULL);
            if (!alt) n = strip_zeros(body, n, n);
        } else {
            n = sci(a, P - 1, alt, upper, body, NULL);
            size_t exp_at = n;
            while (exp_at > 0 && body[exp_at - 1] != 'e' && body[exp_at - 1] != 'E') exp_at--;
            if (exp_at > 0) exp_at--;
            if (!alt) n = strip_zeros(body, n, exp_at);
        }
    }
    if (pad_zeros_after == 0) {
        emit(w, flags, width, true, prefix, plen, 0, body, n);
        return;
    }
    /* Rare: %.40f — fold the extra zeros into the body for one padded field. */
    char wide[64 + 64];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) wide[k++] = body[i];
    for (size_t i = 0; i < pad_zeros_after && k < sizeof wide; i++) wide[k++] = '0';
    emit(w, flags, width, true, prefix, plen, 0, wide, k);
}

/* ── the formatter ─────────────────────────────────────────────────────── */

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIGL };

size_t diag_vsnformat(char *out, size_t cap, const char *fmt, va_list ap, uint32_t *unsupported)
{
    fw_t w = {.out = out, .cap = cap, .len = 0};
    if (out == NULL || cap == 0) return 0;
    if (fmt == NULL) fmt = "";
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&w, *p); continue; }
        const char *spec = p++;
        int flags = 0;
        for (;; p++) {
            if (*p == '-') flags |= F_LEFT;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_ALT;
            else if (*p == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0;
        if (*p == '*') {
            long long wv = va_arg(ap, int);
            if (wv < 0) { flags |= F_LEFT; wv = -wv; }   /* -INT_MIN fits in long long */
            width = clamp_field(wv, unsupported);
            p++;
        } else {
            long long wv = 0;
            while (*p >= '0' && *p <= '9') { if (wv <= FMT_FIELD_MAX) wv = wv * 10 + (*p - '0'); p++; }
            width = clamp_field(wv, unsupported);
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                const long long pv = va_arg(ap, int);
                prec = pv < 0 ? -1 : clamp_field(pv, unsupported);   /* negative = none (C) */
                p++;
            } else {
                long long pv = 0;
                while (*p >= '0' && *p <= '9') { if (pv <= FMT_FIELD_MAX) pv = pv * 10 + (*p - '0'); p++; }
                prec = clamp_field(pv, unsupported);
            }
        }
        int len = LEN_NONE;
        if (*p == 'h') { len = LEN_H; p++; if (*p == 'h') { len = LEN_HH; p++; } }
        else if (*p == 'l') { len = LEN_L; p++; if (*p == 'l') { len = LEN_LL; p++; } }
        else if (*p == 'j') { len = LEN_J; p++; }
        else if (*p == 'z') { len = LEN_Z; p++; }
        else if (*p == 't') { len = LEN_T; p++; }
        else if (*p == 'L') { len = LEN_BIGL; p++; }
        const char conv = *p;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v;
            switch (len) {
            case LEN_HH: v = (signed char)va_arg(ap, int); break;
            case LEN_H:  v = (short)va_arg(ap, int); break;
            case LEN_L:  v = va_arg(ap, long); break;
            case LEN_LL: v = va_arg(ap, long long); break;
            case LEN_J:  v = (int64_t)va_arg(ap, intmax_t); break;
            case LEN_Z: case LEN_T: v = (int64_t)va_arg(ap, ptrdiff_t); break;
            default:     v = va_arg(ap, int); break;
            }
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            emit_int(&w, flags, width, prec, v < 0, mag, 10, false, true, false);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v;
            switch (len) {
            case LEN_HH: v = (unsigned char)va_arg(ap, unsigned int); break;
            case LEN_H:  v = (unsigned short)va_arg(ap, unsigned int); break;
            case LEN_L:  v = va_arg(ap, unsigned long); break;
            case LEN_LL: v = va_arg(ap, unsigned long long); break;
            case LEN_J:  v = (uint64_t)va_arg(ap, uintmax_t); break;
            case LEN_Z: case LEN_T: v = (uint64_t)va_arg(ap, size_t); break;
            default:     v = va_arg(ap, unsigned int); break;
            }
            const unsigned base = conv == 'o' ? 8U : (conv == 'u' ? 10U : 16U);
            emit_int(&w, flags, width, prec, false, v, base, conv == 'X', false, false);
            break;
        }
        case 'p': {
            const uintptr_t v = (uintptr_t)va_arg(ap, void *);
            emit_int(&w, flags & ~F_ZERO, width, -1, false, (uint64_t)v, 16, false, false, true);
            break;
        }
        case 'c':
            if (len == LEN_L) {                   /* wide char: consumed, not rendered */
                (void)va_arg(ap, int);
                if (unsupported) (*unsupported)++;
                put_n(&w, "<%lc>", 5);
            } else {
                const char ch = (char)va_arg(ap, int);
                emit(&w, flags, width, false, "", 0, 0, &ch, 1);
            }
            break;
        case 's': {
            if (len == LEN_L) {                   /* wide string: consumed, not rendered */
                (void)va_arg(ap, void *);
                if (unsupported) (*unsupported)++;
                put_n(&w, "<%ls>", 5);
                break;
            }
            const char *s = va_arg(ap, const char *);
            if (s == NULL) s = "(null)";
            size_t n = 0;
            /* Bound FIRST, then dereference: with "%.*s" the argument need not
             * be NUL-terminated, and s[prec] may be past the end of a buffer
             * (review round 2, finding 1). */
            while ((prec < 0 || n < (size_t)prec) && s[n]) n++;
            emit(&w, flags, width, false, "", 0, 0, s, n);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            const double v = len == LEN_BIGL ? (double)va_arg(ap, long double) : va_arg(ap, double);
            emit_double(&w, flags, width, prec, conv, v);
            break;
        }
        case 'a': case 'A':
            if (len == LEN_BIGL) (void)va_arg(ap, long double);
            else (void)va_arg(ap, double);
            if (unsupported) (*unsupported)++;
            put_n(&w, conv == 'a' ? "<%a>" : "<%A>", 4);
            break;
        case 'n':
            (void)va_arg(ap, void *);             /* never written through */
            if (unsupported) (*unsupported)++;
            break;
        case '%':
            put(&w, '%');
            break;
        default:
            /* Unknown conversion (or a truncated "%" at the end): the sizes of
             * the remaining arguments are unknowable, so stop here, visibly. */
            if (unsupported) (*unsupported)++;
            put_n(&w, spec, (size_t)(p - spec) + (conv ? 1U : 0U));
            put_n(&w, "<?>", 3);
            w.out[w.len] = '\0';
            return w.len;
        }
    }
    w.out[w.len] = '\0';
    return w.len;
}

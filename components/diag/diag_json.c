#include "diag_json.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void diag_jw_init(diag_jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = buf != NULL && cap > 0;
    if (w->ok) buf[0] = '\0';
}

static void put(diag_jw_t *w, const char *p, size_t n)
{
    if (!w->ok) return;
    if (w->len + n >= w->cap) {          /* keep room for the NUL */
        w->ok = false;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

void diag_jw_raw(diag_jw_t *w, const char *fmt, ...)
{
    if (!w->ok) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->ok = false;
        w->buf[w->len] = '\0';           /* drop the partial write */
        return;
    }
    w->len += (size_t)n;
}

/* Length of the well-formed UTF-8 sequence at p (1..4), or 0 if the bytes do
 * not form one within `avail` bytes (RFC 3629: no overlongs, no surrogates,
 * nothing above U+10FFFF). */
static size_t utf8_seq(const unsigned char *p, size_t avail)
{
    unsigned char c = p[0];
    size_t n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
    else return 0;
    if (avail < n) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

void diag_jw_strn(diag_jw_t *w, const char *s, size_t max)
{
    put(w, "\"", 1);
    if (s != NULL) {
        size_t len = strnlen(s, max);
        const unsigned char *p = (const unsigned char *)s;
        size_t i = 0;
        while (i < len && w->ok) {
            unsigned char c = p[i];
            char esc[8];
            if (c == '"' || c == '\\') {
                esc[0] = '\\'; esc[1] = (char)c;
                put(w, esc, 2);
                i++;
            } else if (c < 0x20 || c == 0x7F) {
                snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
                put(w, esc, 6);
                i++;
            } else {
                size_t n = utf8_seq(p + i, len - i);
                if (n == 0) {
                    put(w, "\\ufffd", 6);   /* stray / cut byte: never invalid UTF-8 */
                    i++;
                } else {
                    put(w, (const char *)p + i, n);
                    i += n;
                }
            }
        }
    }
    put(w, "\"", 1);
}

void diag_jw_rewind(diag_jw_t *w, size_t len)
{
    if (w->buf == NULL || len >= w->cap) return;
    w->len = len;
    w->buf[len] = '\0';
    w->ok = true;
}

/*
 * sched_cron.c — 5-field cron → bitmasks, matching, next-fire search.
 *
 * Fields: min hour dom month dow. Syntax: star, a,b lists, a-b ranges, and
 * steps (star/n or a-b/n); three-letter names for month and dow; dow 0 or 7 =
 * Sunday. No L/W/#/@ — field scripts never used them and each is a research
 * project to explain. dom and dow combine with OR when both are restricted,
 * matching vixie cron.
 */

#include "sched_spec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void cerr(char *err, size_t cap, const char *fmt, ...)
{
    if (err == NULL || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

typedef struct {
    const char *s;
    size_t      len, pos;
} pcur_t;

static void pskip(pcur_t *c)
{
    while (c->pos < c->len && c->s[c->pos] == ' ') c->pos++;
}

/* Map a name prefix (first 3 letters, case-insensitive) to a value, or -1. */
static int name_value(const char *s, size_t len, const char *const *names, int n)
{
    if (len < 3) return -1;
    for (int i = 0; i < n; i++) {
        const char *w = names[i];
        bool ok = true;
        for (int j = 0; j < 3; j++) {
            char a = s[j], b = w[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (a != b) { ok = false; break; }
        }
        if (ok && (len == 3 || s[3] < 'a' || (s[3] > 'z' && (s[3] < 'A' || s[3] > 'Z')))) {
            /* exactly 3 letters, or the 4th char is not a letter */
            if (len == 3) return i;
            /* allow full names too ("january") — compare whole word */
            size_t wl = strlen(w);
            if (wl == len) {
                bool full = true;
                for (size_t j = 3; j < wl; j++) {
                    char a = s[j], b = w[j];
                    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
                    if (a != b) { full = false; break; }
                }
                if (full) return i;
            }
            return -1; /* 3-letter prefix of something else is ambiguous */
        }
    }
    return -1;
}

static bool read_number(pcur_t *c, int *out)
{
    if (c->pos >= c->len || c->s[c->pos] < '0' || c->s[c->pos] > '9') return false;
    int v = 0;
    while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') {
        v = v * 10 + (c->s[c->pos] - '0');
        if (v > 100000) return false; /* absurd; stops overflow */
        c->pos++;
    }
    *out = v;
    return true;
}

/* Parse one field into a bitmask. bits are uint64_t-sized; bit index = value.
 * `names` may be NULL; name_value() returns a 0-based index, so callers pass
 * name_base (1 for months, 0 for dow) to shift it into the field's range.
 * restricted = field did not start with '*' (vixie). */
static bool parse_field(pcur_t *c, int vmin, int vmax,
                        const char *const *names, int n_names, int name_base,
                        uint64_t *bits, bool *restricted,
                        char *err, size_t err_cap, const char *field_name)
{
    pskip(c);
    *bits = 0;
    *restricted = false;
    if (c->pos >= c->len) {
        cerr(err, err_cap, "cron: missing %s field", field_name);
        return false;
    }
    if (c->s[c->pos] != '*') *restricted = true;
    for (;;) {
        int a, b;
        if (c->pos < c->len && c->s[c->pos] == '*') {
            c->pos++;
            a = vmin;
            b = vmax;
        } else {
            size_t nstart = c->pos;
            while (c->pos < c->len &&
                   ((c->s[c->pos] >= 'a' && c->s[c->pos] <= 'z') ||
                    (c->s[c->pos] >= 'A' && c->s[c->pos] <= 'Z'))) c->pos++;
            if (c->pos > nstart) {
                if (names == NULL) {
                    cerr(err, err_cap, "cron: names not allowed in %s field", field_name);
                    return false;
                }
                a = name_value(c->s + nstart, c->pos - nstart, names, n_names);
                if (a >= 0) a += name_base;
                if (a < 0) {
                    cerr(err, err_cap, "cron: unknown name in %s field", field_name);
                    return false;
                }
            } else if (!read_number(c, &a)) {
                cerr(err, err_cap, "cron: expected number, '*' or name in %s field",
                     field_name);
                return false;
            }
            b = a;
        }
        if (c->pos < c->len && c->s[c->pos] == '-') {
            c->pos++;
            if (names != NULL) {
                size_t nstart = c->pos;
                while (c->pos < c->len &&
                       ((c->s[c->pos] >= 'a' && c->s[c->pos] <= 'z') ||
                        (c->s[c->pos] >= 'A' && c->s[c->pos] <= 'Z'))) c->pos++;
                if (c->pos > nstart) {
                    b = name_value(c->s + nstart, c->pos - nstart, names, n_names);
                    if (b >= 0) b += name_base;
                    if (b < 0) {
                        cerr(err, err_cap, "cron: unknown name in %s field", field_name);
                        return false;
                    }
                    goto have_range;
                }
            }
            if (!read_number(c, &b)) {
                cerr(err, err_cap, "cron: expected range end in %s field", field_name);
                return false;
            }
        }
have_range:
        /* dow note: no pre-fold of 7 → 0 here (that collapsed `*` = 0-7 to
         * Sunday-only); the bit-setting loop maps v == 7 → bit 0, so `7`,
         * `5-7` and `*` all work. A wrap range like `5-0` is rejected. */
        if (a < vmin || b > vmax || a > b) {
            cerr(err, err_cap, "cron: value out of range %d..%d in %s field",
                 vmin, vmax, field_name);
            return false;
        }
        int step = 1;
        if (c->pos < c->len && c->s[c->pos] == '/') {
            c->pos++;
            if (!read_number(c, &step) || step < 1) {
                cerr(err, err_cap, "cron: step must be ≥ 1 in %s field", field_name);
                return false;
            }
        }
        for (int v = a; v <= b; v += step) {
            int bit = (vmax == 7 && v == 7) ? 0 : v;
            *bits |= 1ULL << bit;
        }
        if (c->pos < c->len && c->s[c->pos] == ',') { c->pos++; continue; }
        break;
    }
    pskip(c);
    return true;
}

esp_err_t sched_cron_parse(const char *expr, sched_cron_t *out,
                           char *err, size_t err_cap)
{
    static const char *const k_months_full[12] = {
        "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december",
    };
    static const char *const k_dow[7] = {
        "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
    };
    if (expr == NULL || out == NULL) {
        cerr(err, err_cap, "cron: NULL expression");
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* exactly five whitespace-separated fields: "*****" is not cron */
    const char *f[5];
    size_t flen[5];
    int nf = 0;
    size_t i = 0, len = strlen(expr);
    while (i < len) {
        while (i < len && (expr[i] == ' ' || expr[i] == '\t')) i++;
        if (i >= len) break;
        if (nf == 5) { nf++; break; } /* a 6th field: mark overflow */
        f[nf] = expr + i;
        size_t st = i;
        while (i < len && expr[i] != ' ' && expr[i] != '\t') i++;
        flen[nf] = i - st;
        nf++;
    }
    if (nf != 5) {
        cerr(err, err_cap, "cron: expected exactly 5 whitespace-separated "
             "fields (min hour dom month dow)");
        return ESP_FAIL;
    }

    /* Reject the vixie extensions where they appear as field syntax, without
     * misfiring on case-insensitive names (JUL, WED): a list piece that is
     * all letters (a range like mon-fri keeps its hyphen) and long enough to
     * be a name is left for the field parser; anything else carrying an
     * L/W/#/@ is the extension syntax, which we do not support. */
    for (int k = 0; k < 5; k++) {
        size_t p0 = 0;
        for (size_t j = 0; j <= flen[k]; j++) {
            if (j < flen[k] && f[k][j] != ',') continue;
            size_t plen = j - p0;
            bool nameish = plen > 0;
            bool has_lw = false;
            for (size_t q = p0; q < j; q++) {
                char ch = f[k][q];
                if (ch == '#' || ch == '@') {
                    cerr(err, err_cap, "cron: L/W/#/@ extensions are not supported");
                    return ESP_FAIL;
                }
                if (ch == 'L' || ch == 'l' || ch == 'W' || ch == 'w') has_lw = true;
                bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
                if (!letter && ch != '-') nameish = false;
            }
            if (has_lw && !(nameish && plen >= 3)) {
                cerr(err, err_cap, "cron: L/W/#/@ extensions are not supported");
                return ESP_FAIL;
            }
            p0 = j + 1;
        }
    }

    static const struct {
        int vmin, vmax, name_base;
        const char *const *names;
        int n_names;
        const char *field;
    } k_fields[5] = {
        { 0, 59, 0, NULL, 0, "minute" },
        { 0, 23, 0, NULL, 0, "hour" },
        { 1, 31, 0, NULL, 0, "day-of-month" },
        { 1, 12, 1, k_months_full, 12, "month" },
        { 0, 7, 0, k_dow, 7, "day-of-week" },
    };
    for (int k = 0; k < 5; k++) {
        pcur_t c = { f[k], flen[k], 0 };
        uint64_t bits;
        bool restricted;
        if (!parse_field(&c, k_fields[k].vmin, k_fields[k].vmax,
                         k_fields[k].names, k_fields[k].n_names,
                         k_fields[k].name_base, &bits, &restricted,
                         err, err_cap, k_fields[k].field)) {
            return ESP_FAIL;
        }
        if (c.pos != c.len) {
            cerr(err, err_cap, "cron: unexpected character in %s field",
                 k_fields[k].field);
            return ESP_FAIL;
        }
        switch (k) {
        case 0: out->min = bits; break;
        case 1: out->hour = (uint32_t)bits; break;
        case 2: out->dom = (uint32_t)bits; out->dom_restricted = restricted ? 1 : 0; break;
        case 3: out->month = (uint16_t)bits; break;
        default: out->dow = (uint8_t)bits; out->dow_restricted = restricted ? 1 : 0; break;
        }
    }
    return ESP_OK;
}

bool sched_cron_matches(const sched_cron_t *c, const sched_tm_like_t *t)
{
    if ((c->min & (1ULL << t->min)) == 0) return false;
    if ((c->hour & (1U << t->hour)) == 0) return false;
    if ((c->month & (1U << t->month)) == 0) return false;
    bool dom_ok = (c->dom & (1U << t->day)) != 0;
    bool dow_ok = (c->dow & (1U << t->wday)) != 0;
    /* vixie rule: OR when both are restricted, AND otherwise */
    if (c->dom_restricted && c->dow_restricted) return dom_ok || dow_ok;
    return dom_ok && dow_ok;
}

/* First set bit in mask at or after `from` (wrapping at nbits), or -1. */
static int next_bit_u64(uint64_t mask, int from, int nbits)
{
    for (int v = from; v < nbits; v++) {
        if (mask & (1ULL << v)) return v;
    }
    return -1;
}

esp_err_t sched_cron_next(const sched_cron_t *c, int64_t after_local, int64_t *out_local)
{
    if (c == NULL || out_local == NULL) return ESP_ERR_INVALID_ARG;
    /* start at the next minute boundary, strictly after `after_local` */
    int64_t t = (after_local / 60) * 60 + 60;
    int y, mo, d, h, mi, wd;
    time_sync_localtime(t, &y, &mo, &d, &h, &mi, NULL, &wd);
    int64_t day0 = t - (int64_t)h * 3600 - (int64_t)mi * 60;

    for (int day = 0; day <= 366; day++) { /* bounded search, per plan */
        int64_t base = day0 + (int64_t)day * 86400;
        time_sync_localtime(base, &y, &mo, &d, NULL, NULL, NULL, &wd);
        if ((c->month & (1U << mo)) == 0) continue;
        bool dom_ok = (c->dom & (1U << d)) != 0;
        bool dow_ok = (c->dow & (1U << wd)) != 0;
        bool day_ok = (c->dom_restricted && c->dow_restricted)
                          ? (dom_ok || dow_ok)
                          : (dom_ok && dow_ok);
        if (!day_ok) continue;
        int h0 = (day == 0) ? h : 0;
        int hh = next_bit_u64(c->hour, h0, 24);
        if (hh < 0) continue;
        int m0 = (day == 0 && hh == h) ? mi : 0;
        int mm = next_bit_u64(c->min, m0, 60);
        while (hh >= 0) {
            if (mm >= 0) {
                int64_t cand = base + (int64_t)hh * 3600 + (int64_t)mm * 60;
                if (cand > after_local) {
                    *out_local = cand;
                    return ESP_OK;
                }
            }
            /* next minute in this hour, else next hour */
            mm = next_bit_u64(c->min, (mm >= 0 ? mm + 1 : m0 + 1), 60);
            if (mm < 0) {
                hh = next_bit_u64(c->hour, hh + 1, 24);
                m0 = 0;
                mm = (hh >= 0) ? next_bit_u64(c->min, 0, 60) : -1;
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}

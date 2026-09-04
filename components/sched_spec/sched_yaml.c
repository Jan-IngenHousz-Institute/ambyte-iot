/*
 * sched_yaml.c — strict YAML subset parser → generic node tree.
 *
 * The file on the device is the authored, commented YAML (design decision 2),
 * so the parser is schema-agnostic and fuzzable: it produces a small node
 * tree in a transient heap arena (freed after compile) and the compiler owns
 * every schedule rule. Accepted constructs and limits are exactly the plan
 * table; anything else is an error with line:col.
 *
 * Column convention: all line/col fields and error positions are 1-based.
 */

#include "sched_spec.h"
#include "sched_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── arena ─────────────────────────────────────────────────────────────── *
 * Bump allocator over a list of heap blocks. Worst case ≈ 512 nodes × 40 B
 * + 8 KiB strings ≈ 28 KiB, bounded by the parser limits; freed wholesale
 * after compile. Blocks are never individually freed, which keeps node
 * pointers stable while the compiler walks the tree. */

typedef struct arena_block {
    struct arena_block *next;
    size_t used, cap;
    char data[];
} arena_block_t;

struct sched_yaml_doc {
    arena_block_t *blocks;
    sched_node_t  *root;
    size_t         string_bytes; /* vs SCHED_YAML_MAX_STRING_BYTES */
    int            node_count;   /* vs SCHED_YAML_MAX_NODES */
};

#define ARENA_ALIGN 8

static void *arena_alloc(sched_yaml_doc_t *doc, size_t size)
{
    size = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    arena_block_t *b = doc->blocks;
    if (b == NULL || b->used + size > b->cap) {
        size_t cap = (b == NULL) ? 4096 : b->cap * 2;
        while (cap < size) cap *= 2;
        arena_block_t *nb = malloc(sizeof(arena_block_t) + cap);
        if (nb == NULL) return NULL;
        nb->next = doc->blocks;
        nb->used = 0;
        nb->cap  = cap;
        doc->blocks = nb;
        b = nb;
    }
    void *p = b->data + b->used;
    b->used += size;
    return p;
}

/* ── lines ───────────────────────────────────────────────────────────── */

typedef struct {
    const char *text;   /* comment-stripped, trailing whitespace trimmed */
    int         len;
    int         indent; /* leading spaces; -1 = blank line */
    int         lineno; /* 1-based */
} line_t;

typedef struct {
    sched_yaml_doc_t *doc;
    line_t           *lines;
    int               nlines, lines_cap, pos;
    bool              failed;
    char             *err;
    size_t            err_cap;
} parser_t;

static void set_err(parser_t *p, int line, int col, const char *fmt, ...)
{
    if (p->failed) return; /* keep the first error: it caused the rest */
    p->failed = true;
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(p->err, p->err_cap, "%d:%d: ", line, col);
    if (n > 0 && (size_t)n < p->err_cap) {
        vsnprintf(p->err + n, p->err_cap - (size_t)n, fmt, ap);
    }
    va_end(ap);
}

static char *arena_str(parser_t *p, const char *src, size_t len, int line, int col)
{
    sched_yaml_doc_t *doc = p->doc;
    if (doc->string_bytes + len + 1 > SCHED_YAML_MAX_STRING_BYTES) {
        set_err(p, line, col, "string pool limit (%d bytes)", SCHED_YAML_MAX_STRING_BYTES);
        return NULL;
    }
    char *dst = arena_alloc(doc, len + 1);
    if (dst == NULL) {
        set_err(p, line, col, "out of memory");
        return NULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    doc->string_bytes += len + 1;
    return dst;
}

static sched_node_t *node_new(parser_t *p, int kind, int line, int col)
{
    if (p->doc->node_count >= SCHED_YAML_MAX_NODES) {
        set_err(p, line, col, "node limit (%d)", SCHED_YAML_MAX_NODES);
        return NULL;
    }
    sched_node_t *n = arena_alloc(p->doc, sizeof(sched_node_t));
    if (n == NULL) {
        set_err(p, line, col, "out of memory");
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->kind = (uint8_t)kind;
    n->line = (uint16_t)line;
    n->col  = (uint16_t)col;
    p->doc->node_count++;
    return n;
}

static bool is_key_char(char c, bool first)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c == '_') return true;
    if (!first && ((c >= '0' && c <= '9') || c == '/' || c == '-')) return true;
    return false;
}

/* Strip the `#`-to-EOL comment (outside quotes) into a scratch copy and
 * reject tabs. The only escape the subset allows is \" inside double quotes. */
static bool prep_line(parser_t *p, line_t *ln, const char *src, int len)
{
    char *buf = arena_alloc(p->doc, (size_t)len + 1);
    if (buf == NULL) {
        set_err(p, ln->lineno, 1, "out of memory");
        return false;
    }
    int out = 0;
    char quote = '\0';
    for (int i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\t') {
            set_err(p, ln->lineno, i + 1, "tab character; indent with spaces only");
            return false;
        }
        if (quote != '\0') {
            /* inside double quotes copy escape pairs verbatim — unescaping is
             * parse_quoted's job; rewriting \" here would close the string
             * early and corrupt both the text and the column map */
            if (quote == '"' && c == '\\' && i + 1 < len) {
                buf[out++] = c;
                buf[out++] = src[++i];
                continue;
            }
            buf[out++] = c;
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; buf[out++] = c; continue; }
        if (c == '#') break; /* comment to end of line */
        buf[out++] = c;
    }
    while (out > 0 && buf[out - 1] == ' ') out--; /* rstrip */
    buf[out] = '\0';
    ln->text = buf;
    ln->len  = out;
    ln->indent = 0;
    while (ln->indent < out && buf[ln->indent] == ' ') ln->indent++;
    if (ln->indent == out) ln->indent = -1; /* blank */
    return true;
}

static const line_t *peek(parser_t *p)
{
    while (p->pos < p->nlines && p->lines[p->pos].indent < 0) p->pos++;
    if (p->pos >= p->nlines) return NULL;
    return &p->lines[p->pos];
}

/* ── scalars ─────────────────────────────────────────────────────────── */

static bool all_digits(const char *s, size_t len)
{
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

/* Tri-state numeric cores shared with the compiler's signed-duration parser
 * (sched_internal.h): 1 = ok, 0 = not this syntax, -1 = numeric shape but
 * out of int64 range. The caller must treat -1 as an error, not fall
 * through to string — a 19+-digit value is a typo, not a name. */
int sched_u64_digits(const char *s, size_t len, int64_t *out)
{
    if (len == 0) return 0;
    int64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        /* magnitude check before the multiply: no signed-overflow UB */
        if (v > (INT64_MAX - (s[i] - '0')) / 10) return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

int sched_duration_ms(const char *s, size_t len, int64_t *out_ms)
{
    size_t mult, dlen;
    if (len >= 3 && s[len - 2] == 'm' && s[len - 1] == 's') {
        mult = 1; dlen = len - 2;
    } else if (len >= 2) {
        switch (s[len - 1]) {
        case 's': mult = 1000; break;
        case 'm': mult = 60000; break;
        case 'h': mult = 3600000; break;
        default: return 0;
        }
        dlen = len - 1;
    } else {
        return 0;
    }
    int64_t v;
    int ri = sched_u64_digits(s, dlen, &v);
    if (ri <= 0) return ri;
    if (v > INT64_MAX / (int64_t)mult) return -1;
    *out_ms = v * (int64_t)mult;
    return 1;
}

/* signed decimal integer on top of the shared magnitude core */
static int parse_int64(const char *s, size_t len, int64_t *out)
{
    size_t i = 0;
    bool neg = false;
    if (i < len && s[i] == '-') { neg = true; i++; }
    int64_t v;
    int ri = sched_u64_digits(s + i, len - i, &v);
    if (ri <= 0) return ri;
    *out = neg ? -v : v;
    return 1;
}

/* duration: [0-9]+(ms|s|m|h) → milliseconds, via the shared core */
static int parse_duration(const char *s, size_t len, int64_t *out_ms)
{
    return sched_duration_ms(s, len, out_ms);
}

static bool ieq(const char *s, size_t len, const char *word)
{
    size_t wl = strlen(word);
    if (wl != len) return false;
    for (size_t i = 0; i < len; i++) {
        char a = s[i], b = word[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static sched_node_t *scalar_node(parser_t *p, int line, int col)
{
    sched_node_t *n = node_new(p, SCHED_NODE_SCALAR, line, col);
    return n;
}

static bool store_str(parser_t *p, sched_node_t *n, const char *s, size_t len,
                      int line, int col)
{
    char *copy = arena_str(p, s, len, line, col);
    if (copy == NULL) return false;
    n->u.s.str = copy;
    return true;
}

/* Resolve a plain (unquoted) scalar per the plan's order: bool → int →
 * float → duration → HH:MM → plain string (quoted strings are handled
 * earlier). Rejected constructs are errors, never strings. */
static sched_node_t *resolve_plain(parser_t *p, const char *s, size_t len,
                                   int line, int col)
{
    char c0 = len > 0 ? s[0] : '\0';
    if (c0 == '&') { set_err(p, line, col, "anchors are not supported"); return NULL; }
    if (c0 == '*') { set_err(p, line, col, "aliases are not supported"); return NULL; }
    if (c0 == '!') { set_err(p, line, col, "tags are not supported"); return NULL; }
    if (c0 == '|' || c0 == '>') {
        set_err(p, line, col, "block scalars (| >) are not supported");
        return NULL;
    }
    /* YAML 1.1 booleans: rejected so a hand-edited file cannot silently mean
     * something other than what a non-YAML reader sees. */
    if (ieq(s, len, "yes") || ieq(s, len, "no") ||
        ieq(s, len, "on")  || ieq(s, len, "off")) {
        set_err(p, line, col, "'%.*s' is not a boolean here; use true/false",
                (int)len, s);
        return NULL;
    }

    sched_node_t *n = scalar_node(p, line, col);
    if (n == NULL) return NULL;

    int64_t iv;
    int ri = parse_int64(s, len, &iv);
    if (ieq(s, len, "true") || ieq(s, len, "false")) {
        n->scal_kind = SCHED_SCAL_BOOL;
        n->u.s.b = (s[0] == 't' || s[0] == 'T') ? 1 : 0;
    } else if (ri == 1) {
        n->scal_kind = SCHED_SCAL_INT;
        n->u.s.i = iv;
    } else if (ri < 0) {
        /* numeric-shaped but out of int64 range: a typo, not a name */
        set_err(p, line, col, "integer out of range: '%.*s'", (int)len, s);
        return NULL;
    } else {
        /* float: exactly -?[0-9]+\.[0-9]+ (no exponent; hand-parsed so the
         * result is locale-independent) */
        size_t start = (len > 0 && s[0] == '-') ? 1 : 0;
        size_t dot = 0;
        bool shape = start < len;
        for (size_t i = start; shape && i < len; i++) {
            if (s[i] == '.' && dot == 0) { dot = i; continue; }
            if (s[i] < '0' || s[i] > '9') shape = false;
        }
        if (shape && dot > start && dot + 1 < len) {
            double v = 0.0;
            for (size_t i = start; i < dot; i++) v = v * 10.0 + (double)(s[i] - '0');
            double scale = 0.1;
            for (size_t i = dot + 1; i < len; i++) {
                v += (double)(s[i] - '0') * scale;
                scale /= 10.0;
            }
            n->scal_kind = SCHED_SCAL_FLOAT;
            n->u.s.f = (start == 1) ? -v : v;
        } else {
            int64_t ms;
            int rd = parse_duration(s, len, &ms);
            if (rd == 1) {
                n->scal_kind = SCHED_SCAL_DURATION_MS;
                n->u.s.ms = ms;
            } else if (rd < 0) {
                set_err(p, line, col, "duration out of range: '%.*s'", (int)len, s);
                return NULL;
            } else {
                const char *colon = memchr(s, ':', len);
                if (colon != NULL) {
                    size_t hlen = (size_t)(colon - s);
                    size_t mlen = len - hlen - 1;
                    int64_t hh = 0, mm = 0;
                    bool hhmm = hlen >= 1 && hlen <= 2 && mlen == 2 &&
                                all_digits(s, hlen) && all_digits(colon + 1, mlen);
                    if (hhmm) {
                        (void)parse_int64(s, hlen, &hh);       /* ≤ 2 digits: cannot fail */
                        (void)parse_int64(colon + 1, mlen, &mm);
                        /* an out-of-range clock shape is an error, not a
                         * plain string: 25:00 is a typo, not a name */
                        if (hh > 23 || mm > 59) {
                            set_err(p, line, col,
                                    "invalid time %.*s (want HH:MM, 00:00–23:59)",
                                    (int)len, s);
                            return NULL;
                        }
                        n->scal_kind = SCHED_SCAL_HHMM;
                        n->u.s.hh = (int)hh;
                        n->u.s.mm = (int)mm;
                        return store_str(p, n, s, len, line, col) ? n : NULL;
                    }
                }
                n->scal_kind = SCHED_SCAL_STR;
            }
        }
    }
    return store_str(p, n, s, len, line, col) ? n : NULL;
}

/* ── flow collections and inline values ──────────────────────────────── *
 * cursor_t: col0 is the 1-based column of s[0]; the column of s[pos] is
 * col0 + pos. */

typedef struct {
    const char *s;
    size_t      len, pos;
    int         line, col0;
} cursor_t;

static void skip_ws(cursor_t *c)
{
    while (c->pos < c->len && c->s[c->pos] == ' ') c->pos++;
}

static sched_node_t *parse_flow_value(parser_t *p, cursor_t *c, int depth);

static sched_node_t *parse_quoted(parser_t *p, cursor_t *c)
{
    char quote = c->s[c->pos];
    int str_col = c->col0 + (int)c->pos;
    c->pos++;
    char *buf = arena_alloc(p->doc, c->len - c->pos + 1);
    if (buf == NULL) { set_err(p, c->line, str_col, "out of memory"); return NULL; }
    size_t out = 0;
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (quote == '"' && ch == '\\') {
            /* the only supported escape is \" — anything else (\n, \\,
             * \uXXXX) is an error, so a file cannot silently mean something
             * other than what a non-YAML reader sees */
            if (c->pos + 1 < c->len && c->s[c->pos + 1] == '"') {
                ch = '"';
                c->pos += 2;
            } else {
                set_err(p, c->line, c->col0 + (int)c->pos,
                        "unsupported escape (only \\\" is allowed inside "
                        "double quotes)");
                return NULL;
            }
        } else if (ch == quote) {
            c->pos++;
            /* the terminating NUL costs a pool byte too — an empty scalar
             * reaches this branch without any per-byte check having run */
            if (p->doc->string_bytes + out + 1 > SCHED_YAML_MAX_STRING_BYTES) {
                set_err(p, c->line, str_col, "string pool limit (%d bytes)",
                        SCHED_YAML_MAX_STRING_BYTES);
                return NULL;
            }
            buf[out] = '\0';
            p->doc->string_bytes += out + 1; /* budget pre-checked per byte */
            sched_node_t *n = scalar_node(p, c->line, str_col);
            if (n == NULL) return NULL;
            n->scal_kind = SCHED_SCAL_STR;
            n->u.s.str = buf;
            return n;
        } else {
            c->pos++;
        }
        /* enforce the string budget BEFORE the write: this byte plus the
         * eventual NUL must fit what is left of the pool */
        if (p->doc->string_bytes + out + 2 > SCHED_YAML_MAX_STRING_BYTES) {
            set_err(p, c->line, str_col, "string pool limit (%d bytes)",
                    SCHED_YAML_MAX_STRING_BYTES);
            return NULL;
        }
        buf[out++] = ch;
    }
    set_err(p, c->line, str_col, "unterminated quoted string");
    return NULL;
}

static bool grow_pairs(parser_t *p, sched_node_t *n, int line, int col)
{
    if ((n->u.m.count & 3) != 0) return true; /* chunks of 4 */
    sched_pair_t *np = arena_alloc(p->doc, (size_t)(n->u.m.count + 4) * sizeof(sched_pair_t));
    if (np == NULL) { set_err(p, line, col, "out of memory"); return false; }
    if (n->u.m.pairs != NULL) {
        memcpy(np, n->u.m.pairs, (size_t)n->u.m.count * sizeof(sched_pair_t));
    }
    n->u.m.pairs = np;
    return true;
}

static bool grow_items(parser_t *p, sched_node_t *n, int line, int col)
{
    if ((n->u.q.count & 3) != 0) return true;
    sched_node_t **ni = arena_alloc(p->doc, (size_t)(n->u.q.count + 4) * sizeof(sched_node_t *));
    if (ni == NULL) { set_err(p, line, col, "out of memory"); return false; }
    if (n->u.q.items != NULL) {
        memcpy(ni, n->u.q.items, (size_t)n->u.q.count * sizeof(sched_node_t *));
    }
    n->u.q.items = ni;
    return true;
}

static sched_node_t *parse_flow_map(parser_t *p, cursor_t *c, int depth)
{
    /* collections self-check their depth before allocating; children recurse
     * with depth + 1 and are checked the same way on entry */
    if (depth > SCHED_YAML_MAX_DEPTH) {
        set_err(p, c->line, c->col0 + (int)c->pos,
                "nesting deeper than %d", SCHED_YAML_MAX_DEPTH);
        return NULL;
    }
    sched_node_t *n = node_new(p, SCHED_NODE_MAP, c->line, c->col0 + (int)c->pos);
    if (n == NULL) return NULL;
    c->pos++; /* '{' */
    skip_ws(c);
    if (c->pos < c->len && c->s[c->pos] == '}') { c->pos++; return n; }
    while (c->pos < c->len) {
        skip_ws(c);
        size_t kstart = c->pos;
        while (c->pos < c->len && is_key_char(c->s[c->pos], c->pos == kstart)) c->pos++;
        size_t klen = c->pos - kstart;
        if (klen == 0) {
            set_err(p, c->line, c->col0 + (int)c->pos, "expected key in flow mapping");
            return NULL;
        }
        skip_ws(c);
        if (c->pos >= c->len || c->s[c->pos] != ':') {
            set_err(p, c->line, c->col0 + (int)c->pos, "expected ':' in flow mapping");
            return NULL;
        }
        c->pos++;
        if (c->pos < c->len && c->s[c->pos] != ' ' &&
            c->s[c->pos] != '}' && c->s[c->pos] != ',') {
            set_err(p, c->line, c->col0 + (int)c->pos, "expected space after ':'");
            return NULL;
        }
        sched_node_t *val = parse_flow_value(p, c, depth + 1);
        if (val == NULL) return NULL;
        for (int i = 0; i < n->u.m.count; i++) {
            if (strlen(n->u.m.pairs[i].key) == klen &&
                memcmp(n->u.m.pairs[i].key, c->s + kstart, klen) == 0) {
                set_err(p, c->line, c->col0 + (int)kstart, "duplicate key '%.*s'",
                        (int)klen, c->s + kstart);
                return NULL;
            }
        }
        char *key = arena_str(p, c->s + kstart, klen, c->line, c->col0 + (int)kstart);
        if (key == NULL) return NULL;
        if (!grow_pairs(p, n, c->line, c->col0)) return NULL;
        n->u.m.pairs[n->u.m.count].key = key;
        n->u.m.pairs[n->u.m.count].value = val;
        n->u.m.count++;
        skip_ws(c);
        if (c->pos < c->len && c->s[c->pos] == ',') { c->pos++; continue; }
        if (c->pos < c->len && c->s[c->pos] == '}') { c->pos++; return n; }
        set_err(p, c->line, c->col0 + (int)c->pos, "expected ',' or '}' in flow mapping");
        return NULL;
    }
    set_err(p, c->line, c->col0 + (int)c->pos,
            "unterminated flow mapping (multi-line flow is not supported)");
    return NULL;
}

static sched_node_t *parse_flow_seq(parser_t *p, cursor_t *c, int depth)
{
    if (depth > SCHED_YAML_MAX_DEPTH) {
        set_err(p, c->line, c->col0 + (int)c->pos,
                "nesting deeper than %d", SCHED_YAML_MAX_DEPTH);
        return NULL;
    }
    sched_node_t *n = node_new(p, SCHED_NODE_SEQ, c->line, c->col0 + (int)c->pos);
    if (n == NULL) return NULL;
    c->pos++; /* '[' */
    skip_ws(c);
    if (c->pos < c->len && c->s[c->pos] == ']') { c->pos++; return n; }
    while (c->pos < c->len) {
        sched_node_t *item = parse_flow_value(p, c, depth + 1);
        if (item == NULL) return NULL;
        if (!grow_items(p, n, c->line, c->col0)) return NULL;
        n->u.q.items[n->u.q.count++] = item;
        skip_ws(c);
        if (c->pos < c->len && c->s[c->pos] == ',') { c->pos++; continue; }
        if (c->pos < c->len && c->s[c->pos] == ']') { c->pos++; return n; }
        set_err(p, c->line, c->col0 + (int)c->pos, "expected ',' or ']' in flow sequence");
        return NULL;
    }
    set_err(p, c->line, c->col0 + (int)c->pos,
            "unterminated flow sequence (multi-line flow is not supported)");
    return NULL;
}

static sched_node_t *parse_flow_value(parser_t *p, cursor_t *c, int depth)
{
    skip_ws(c);
    if (c->pos >= c->len) {
        set_err(p, c->line, c->col0 + (int)c->pos, "missing value");
        return NULL;
    }
    char ch = c->s[c->pos];
    if (ch == '{') return parse_flow_map(p, c, depth);
    if (ch == '[') return parse_flow_seq(p, c, depth);
    if (ch == '"' || ch == '\'') return parse_quoted(p, c);
    /* plain scalar inside flow: up to , } ] */
    size_t start = c->pos;
    while (c->pos < c->len) {
        char k = c->s[c->pos];
        if (k == ',' || k == '}' || k == ']') break;
        c->pos++;
    }
    size_t end = c->pos;
    while (end > start && c->s[end - 1] == ' ') end--;
    if (end == start) {
        set_err(p, c->line, c->col0 + (int)start, "missing value");
        return NULL;
    }
    return resolve_plain(p, c->s + start, end - start, c->line, c->col0 + (int)start);
}

/* Whole-line inline value (after `key:` or `- `): flow collection, quoted
 * string, or plain scalar to end of line. col = 1-based column of s[0].
 * depth = the containing block collection's depth: a flow collection on the
 * key's (or dash's) own line adds no indentation level, so it occupies that
 * same depth and self-checks it on entry; each further nested flow collection
 * costs +1 via parse_flow_value. Scalars are leaves and uncounted. One depth
 * counter thus runs across block and flow nesting. */
static sched_node_t *parse_inline(parser_t *p, const char *s, size_t len,
                                  int line, int col, int depth)
{
    cursor_t c = { s, len, 0, line, col };
    skip_ws(&c);
    if (c.pos >= c.len) {
        set_err(p, line, col, "missing value");
        return NULL;
    }
    char ch = c.s[c.pos];
    sched_node_t *n;
    if (ch == '{') {
        n = parse_flow_map(p, &c, depth);
    } else if (ch == '[') {
        n = parse_flow_seq(p, &c, depth);
    } else if (ch == '"' || ch == '\'') {
        n = parse_quoted(p, &c);
    } else {
        return resolve_plain(p, c.s + c.pos, c.len - c.pos, line, col + (int)c.pos);
    }
    if (n == NULL) return NULL;
    skip_ws(&c);
    if (c.pos < c.len) {
        set_err(p, line, c.col0 + (int)c.pos,
                "unexpected content after value");
        return NULL;
    }
    return n;
}

/* ── block structure ─────────────────────────────────────────────────── */

static sched_node_t *parse_block(parser_t *p, int min_indent, int depth);

/* Block mapping with keys at column ind+1. `first` optionally carries the
 * text of the first pair for the `- key: …` inline-mapping case (already
 * consumed from the line array); following sibling keys come from peek(). */
static sched_node_t *parse_mapping(parser_t *p, int ind, int depth,
                                   const char *first, int first_len,
                                   int first_lineno, int first_col)
{
    int line0 = first_lineno;
    if (first == NULL) {
        const line_t *ln = peek(p);
        line0 = ln ? ln->lineno : 0;
    }
    sched_node_t *map = node_new(p, SCHED_NODE_MAP, line0, ind + 1);
    if (map == NULL) return NULL;

    bool have_first = first != NULL;
    for (;;) {
        const char *content;
        int content_len, lineno, content_col;
        if (have_first) {
            content = first;
            content_len = first_len;
            lineno = first_lineno;
            content_col = first_col;
        } else {
            const line_t *ln = peek(p);
            if (ln == NULL || ln->indent < ind) break;
            if (ln->indent > ind) {
                set_err(p, ln->lineno, ln->indent + 1,
                        "unexpected indentation (mapping keys align at column %d)",
                        ind + 1);
                return NULL;
            }
            if (ln->text[ln->indent] == '-') break; /* sibling sequence item */
            content = ln->text + ln->indent;
            content_len = ln->len - ln->indent;
            lineno = ln->lineno;
            content_col = ln->indent + 1;
        }

        /* key */
        int i = 0;
        while (i < content_len && is_key_char(content[i], i == 0)) i++;
        if (i == 0 || i >= content_len || content[i] != ':') {
            set_err(p, lineno, content_col + i, "expected 'key:'");
            return NULL;
        }
        int klen = i;
        if (i + 1 < content_len && content[i + 1] != ' ') {
            set_err(p, lineno, content_col + i + 1, "expected space after ':'");
            return NULL;
        }
        for (int k = 0; k < map->u.m.count; k++) {
            if ((int)strlen(map->u.m.pairs[k].key) == klen &&
                memcmp(map->u.m.pairs[k].key, content, (size_t)klen) == 0) {
                set_err(p, lineno, content_col, "duplicate key '%.*s'", klen, content);
                return NULL;
            }
        }
        const char *rest = content + i + 1;
        int rest_len = content_len - i - 1;
        while (rest_len > 0 && *rest == ' ') { rest++; rest_len--; }
        int rest_col = content_col + (content_len - rest_len);

        sched_node_t *val;
        if (rest_len == 0) {
            if (!have_first) p->pos++;
            /* nested block must be indented deeper than the key */
            val = parse_block(p, ind + 1, depth + 1);
            if (val == NULL && !p->failed) {
                set_err(p, lineno, content_col + klen, "key '%.*s' has no value",
                        klen, content);
                return NULL;
            }
            if (val == NULL) return NULL;
        } else {
            val = parse_inline(p, rest, (size_t)rest_len, lineno, rest_col,
                               depth);
            if (val == NULL) return NULL;
            if (!have_first) p->pos++;
        }
        have_first = false;

        char *key = arena_str(p, content, (size_t)klen, lineno, content_col);
        if (key == NULL) return NULL;
        if (!grow_pairs(p, map, lineno, content_col)) return NULL;
        map->u.m.pairs[map->u.m.count].key = key;
        map->u.m.pairs[map->u.m.count].value = val;
        map->u.m.count++;
    }
    return map;
}

static sched_node_t *parse_sequence(parser_t *p, int ind, int depth)
{
    const line_t *ln0 = peek(p);
    sched_node_t *seq = node_new(p, SCHED_NODE_SEQ, ln0 ? ln0->lineno : 0, ind + 1);
    if (seq == NULL) return NULL;

    for (;;) {
        const line_t *ln = peek(p);
        if (ln == NULL || ln->indent != ind) break;
        const char *content = ln->text + ln->indent;
        int content_len = ln->len - ln->indent;
        if (content[0] != '-') break;
        if (content_len > 1 && content[1] != ' ') {
            set_err(p, ln->lineno, ind + 2, "expected space after '-'");
            return NULL;
        }
        const char *rest = content + 1;
        int rest_len = content_len - 1;
        while (rest_len > 0 && *rest == ' ') { rest++; rest_len--; }
        int rest_col = (ln->indent + 1) + (content_len - rest_len); /* 1-based */

        if (!grow_items(p, seq, ln->lineno, ind + 1)) return NULL;

        sched_node_t *item;
        if (rest_len == 0) {
            p->pos++;
            item = parse_block(p, ind + 1, depth + 1);
            if (item == NULL && !p->failed) {
                set_err(p, ln->lineno, ind + 1, "empty sequence item");
                return NULL;
            }
            if (item == NULL) return NULL;
        } else if (rest[0] == '{' || rest[0] == '[') {
            item = parse_inline(p, rest, (size_t)rest_len, ln->lineno, rest_col,
                                depth);
            if (item == NULL) return NULL;
            p->pos++;
        } else if (is_key_char(rest[0], true)) {
            /* `- uses: x` — inline mapping start (GitHub Actions step shape);
             * sibling keys follow at the key's column. Consume the dash line
             * first so parse_mapping sees only the siblings. */
            int klen = 0;
            while (klen < rest_len && is_key_char(rest[klen], klen == 0)) klen++;
            if (klen < rest_len && rest[klen] == ':' &&
                (klen + 1 == rest_len || rest[klen + 1] == ' ')) {
                int key_col0 = rest_col - 1; /* 0-based */
                p->pos++;
                item = parse_mapping(p, key_col0, depth, rest, rest_len,
                                     ln->lineno, rest_col);
                if (item == NULL) return NULL;
            } else {
                /* scalar item: route through the inline parser so quoted
                 * items get the same grammar as mapping values */
                item = parse_inline(p, rest, (size_t)rest_len, ln->lineno,
                                    rest_col, depth);
                if (item == NULL) return NULL;
                p->pos++;
            }
        } else {
            item = parse_inline(p, rest, (size_t)rest_len, ln->lineno, rest_col,
                                depth);
            if (item == NULL) return NULL;
            p->pos++;
        }
        seq->u.q.items[seq->u.q.count++] = item;
    }
    return seq;
}

/* Parse the block starting at the next non-blank line, requiring indent ≥
 * min_indent. NULL without failing = "nothing at this level" (nested-block
 * probe). */
static sched_node_t *parse_block(parser_t *p, int min_indent, int depth)
{
    const line_t *ln = peek(p);
    if (ln == NULL || ln->indent < min_indent) return NULL;
    if (depth > SCHED_YAML_MAX_DEPTH) {
        set_err(p, ln->lineno, ln->indent + 1,
                "nesting deeper than %d", SCHED_YAML_MAX_DEPTH);
        return NULL;
    }
    int ind = ln->indent;
    if (ln->text[ind] == '-') {
        return parse_sequence(p, ind, depth);
    }
    return parse_mapping(p, ind, depth, NULL, 0, 0, 0);
}

/* ── entry point ─────────────────────────────────────────────────────── */

esp_err_t sched_yaml_parse(const char *text, size_t len,
                           sched_yaml_doc_t **out, char *err, size_t err_cap)
{
    if (err != NULL && err_cap > 0) err[0] = '\0';
    if (text == NULL || out == NULL || err == NULL || err_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (len > SCHED_YAML_MAX_FILE_BYTES) {
        snprintf(err, err_cap, "1:1: file exceeds %d bytes", SCHED_YAML_MAX_FILE_BYTES);
        return ESP_FAIL;
    }
    sched_yaml_doc_t *doc = calloc(1, sizeof(*doc));
    if (doc == NULL) {
        snprintf(err, err_cap, "1:1: out of memory");
        return ESP_FAIL;
    }
    parser_t p;
    memset(&p, 0, sizeof(p));
    p.doc = doc;
    p.err = err;
    p.err_cap = err_cap;

    /* split into lines; enforce line length and reject document markers */
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t llen = i - start;
        if (llen > 0 && text[start + llen - 1] == '\r') llen--; /* CRLF tolerant */
        if (llen > SCHED_YAML_MAX_LINE) {
            set_err(&p, p.nlines + 1, SCHED_YAML_MAX_LINE + 1,
                    "line exceeds %d chars", SCHED_YAML_MAX_LINE);
            goto fail;
        }
        if (p.nlines == p.lines_cap) {
            p.lines_cap = p.lines_cap == 0 ? 32 : p.lines_cap * 2;
            line_t *nl = realloc(p.lines, (size_t)p.lines_cap * sizeof(line_t));
            if (nl == NULL) {
                set_err(&p, p.nlines + 1, 1, "out of memory");
                goto fail;
            }
            p.lines = nl;
        }
        line_t *ln = &p.lines[p.nlines++];
        ln->lineno = p.nlines;
        if (!prep_line(&p, ln, text + start, (int)llen)) goto fail;
        if (ln->indent >= 0 &&
            (strncmp(ln->text + ln->indent, "---", 3) == 0 ||
             strncmp(ln->text + ln->indent, "...", 3) == 0)) {
            set_err(&p, ln->lineno, ln->indent + 1,
                    "multi-document markers (--- / ...) are not supported");
            goto fail;
        }
        if (i < len) i++; /* skip '\n' */
    }

    p.pos = 0;
    doc->root = parse_block(&p, 0, 1);
    if (p.failed) goto fail;
    if (doc->root == NULL) {
        set_err(&p, 1, 1, "empty document");
        goto fail;
    }
    {
        const line_t *ln = peek(&p);
        if (ln != NULL) {
            set_err(&p, ln->lineno, ln->indent + 1,
                    "unexpected content after document");
            goto fail;
        }
    }
    free(p.lines);
    *out = doc;
    return ESP_OK;

fail:
    free(p.lines);
    sched_yaml_free(doc);
    return ESP_FAIL;
}

const sched_node_t *sched_yaml_root(const sched_yaml_doc_t *doc)
{
    return doc != NULL ? doc->root : NULL;
}

void sched_yaml_free(sched_yaml_doc_t *doc)
{
    if (doc == NULL) return;
    arena_block_t *b = doc->blocks;
    while (b != NULL) {
        arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(doc);
}

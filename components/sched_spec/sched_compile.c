/*
 * sched_compile.c — node tree → bounded, validated sched_program_t.
 *
 * Every schedule rule lives here (the parser stays schema-agnostic). The
 * list in plan/yaml-and-compiler "Compiler rules" is the contract and both
 * CI and the device run this same function. Strict: unknown keys are errors.
 * Errors are "line:col: message" — the same string on device logs,
 * `schedule validate`, the script_status failure detail and CI.
 */

#include "sched_spec.h"
#include "sched_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SOLAR_OFFSET_MAX_S (12 * 3600)
#define TRACE_MARGIN_DEFAULT_MS 15000 /* matches the action table default */

typedef struct {
    sched_program_t    *prog;
    const sched_node_t *protocols_node; /* gathered by compile_header */
    const sched_node_t *jobs_node;
    bool                failed;
    char               *err;
    size_t              err_cap;
} ctx_t;

static void cerr(ctx_t *c, const sched_node_t *n, const char *fmt, ...)
{
    if (c->failed) return;
    c->failed = true;
    va_list ap;
    va_start(ap, fmt);
    int written = snprintf(c->err, c->err_cap, "%d:%d: ",
                           n != NULL ? n->line : 1, n != NULL ? n->col : 1);
    if (written > 0 && (size_t)written < c->err_cap) {
        vsnprintf(c->err + written, c->err_cap - (size_t)written, fmt, ap);
    }
    va_end(ap);
}

/* ── node helpers ────────────────────────────────────────────────────── */

static const sched_node_t *map_get(const sched_node_t *map, const char *key)
{
    if (map == NULL || map->kind != SCHED_NODE_MAP) return NULL;
    for (int i = 0; i < map->u.m.count; i++) {
        if (strcmp(map->u.m.pairs[i].key, key) == 0) return map->u.m.pairs[i].value;
    }
    return NULL;
}

/* The node's display text: .str is kept for every scalar kind. */
static const char *node_text(const sched_node_t *n)
{
    return (n != NULL && n->kind == SCHED_NODE_SCALAR) ? n->u.s.str : NULL;
}

/* ── program pool / entries ──────────────────────────────────────────── */

static uint16_t pool_add(ctx_t *c, const sched_node_t *n, const char *s)
{
    size_t len = strlen(s);
    sched_program_t *p = c->prog;
    if (p->pool_used + len + 1 > SCHED_SPEC_STRING_POOL) {
        cerr(c, n, "program string pool exhausted (%d bytes; names, tags, "
                   "messages — see SCHED_SPEC_STRING_POOL)", SCHED_SPEC_STRING_POOL);
        return SCHED_POOL_NONE;
    }
    uint16_t off = p->pool_used;
    memcpy(p->pool + off, s, len + 1);
    p->pool_used += (uint16_t)(len + 1);
    return off;
}

const char *sched_pool_str(const sched_program_t *p, uint16_t off)
{
    if (p == NULL || off == SCHED_POOL_NONE || off >= p->pool_used) return NULL;
    return p->pool + off;
}

static sched_entry_t *entry_add(ctx_t *c, const sched_node_t *n)
{
    sched_program_t *p = c->prog;
    if (p->entry_count >= SCHED_SPEC_MAX_ENTRIES) {
        cerr(c, n, "entry pool exhausted (%d typed values; see SCHED_SPEC_MAX_ENTRIES)",
             SCHED_SPEC_MAX_ENTRIES);
        return NULL;
    }
    return &p->entries[p->entry_count++];
}

/* ── document header ─────────────────────────────────────────────────── *
 * ALL document-header handling lives in this one function (single call site
 * in sched_compile): the schema discriminator gate, the JII provenance keys
 * and name/description. Adding or renaming a header field (the platform's
 * workbook integration is still settling the exact idiom) is a change here
 * and nowhere else. */

/* name/filename fit the publish-envelope snapshot fields (sched_header_t's
 * char[48]) — a longer string would silently truncate on the wire. */
#define MACRO_FIELD_CAP 47

/* 8-4-4-4-12 hex (either case). The platform keys macro execution per row on
 * this id, so a mistyped id must fail at install/CI time, not at ingest. */
static bool macro_id_is_uuid(const char *s)
{
    static const int k_groups[] = { 8, 4, 4, 4, 12 };
    size_t pos = 0;
    for (int g = 0; g < 5; g++) {
        for (int i = 0; i < k_groups[g]; i++) {
            char ch = s[pos++];
            bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                       (ch >= 'A' && ch <= 'F');
            if (!hex) return false;
        }
        if (g < 4 && s[pos++] != '-') return false;
    }
    return s[pos] == '\0';
}

/* The publish envelope splices macro strings into the JSON verbatim — an
 * escaping pass on every publish is not worth its code size when the set of
 * legitimate characters is this small. Restricting the alphabet HERE, at
 * compile time, is what makes the unescaped splice provably safe. */
static bool macro_str_safe(const char *s)
{
    if (*s == '\0') return false;
    for (; *s != '\0'; s++) {
        char ch = *s;
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
                  ch == ':' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

/* Optional top-level `macros:` block — the workbook macro cells the installer
 * stamps into the header (see plan "workbook → device → macro → dashboard").
 * The firmware carries the list as provenance and publishes it verbatim in
 * the envelope; it never acts on it, like the other header keys. */
static bool compile_macros(ctx_t *c, const sched_node_t *node)
{
    if (node->kind != SCHED_NODE_SEQ) {
        cerr(c, node, "'macros' must be a block sequence of "
             "{id, name, filename} mappings");
        return false;
    }
    if (node->u.q.count > SCHED_SPEC_MAX_MACROS) {
        cerr(c, node, "'macros' has %d entries; the cap is %d "
             "(see SCHED_SPEC_MAX_MACROS)", node->u.q.count, SCHED_SPEC_MAX_MACROS);
        return false;
    }
    for (int i = 0; i < node->u.q.count; i++) {
        const sched_node_t *item = node->u.q.items[i];
        if (item->kind != SCHED_NODE_MAP) {
            cerr(c, item, "macro entry must be a mapping {id, name, filename}");
            return false;
        }
        const sched_node_t *vals[3];
        static const char *const k_keys[] = { "id", "name", "filename" };
        for (int f = 0; f < 3; f++) vals[f] = map_get(item, k_keys[f]);
        if (vals[0] == NULL || vals[1] == NULL || vals[2] == NULL) {
            cerr(c, item, "macro entry requires id, name and filename");
            return false;
        }
        for (int k = 0; k < item->u.m.count; k++) {
            const char *key = item->u.m.pairs[k].key;
            if (strcmp(key, "id") != 0 && strcmp(key, "name") != 0 &&
                strcmp(key, "filename") != 0) {
                cerr(c, item->u.m.pairs[k].value, "unknown macro key '%s'", key);
                return false;
            }
        }
        sched_macro_t *m = &c->prog->macros[c->prog->macro_count];
        uint16_t *offs[3] = { &m->id_off, &m->name_off, &m->filename_off };
        for (int f = 0; f < 3; f++) {
            const sched_node_t *v = vals[f];
            /* same string-scalar rule as the other header keys: every scalar
             * keeps .str, so 123 / true / 30m must not compile */
            if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_STR) {
                cerr(c, v, "macro '%s' must be a string", k_keys[f]);
                return false;
            }
            const char *s = v->u.s.str;
            if (f == 0 && !macro_id_is_uuid(s)) {
                cerr(c, v, "macro 'id' must be a uuid (8-4-4-4-12 hex)");
                return false;
            }
            if (f > 0 && strlen(s) > MACRO_FIELD_CAP) {
                cerr(c, v, "macro '%s' is %d chars; the envelope snapshot caps "
                     "it at %d", k_keys[f], (int)strlen(s), MACRO_FIELD_CAP);
                return false;
            }
            if (!macro_str_safe(s)) {
                cerr(c, v, "macro '%s' may only contain [A-Za-z0-9_.:-] "
                     "(spliced into the publish envelope unescaped)", k_keys[f]);
                return false;
            }
            *offs[f] = pool_add(c, v, s);
            if (c->failed) return false;
        }
        c->prog->macro_count++;
    }
    return true;
}

static bool compile_header(ctx_t *c, const sched_node_t *root)
{
    if (root->kind != SCHED_NODE_MAP) {
        cerr(c, root, "document root must be a mapping");
        return false;
    }
    const sched_node_t *schema = map_get(root, "schema");
    if (schema == NULL) {
        cerr(c, root, "missing required header key 'schema: %s'",
             SCHED_SPEC_SCHEMA_CONST);
        return false;
    }
    const char *sv = node_text(schema);
    if (sv == NULL || strcmp(sv, SCHED_SPEC_SCHEMA_CONST) != 0) {
        cerr(c, schema, "unsupported schema '%s'; expected '%s'",
             sv != NULL ? sv : "?", SCHED_SPEC_SCHEMA_CONST);
        return false;
    }
    static const struct { const char *key; uint16_t *off; } k_provenance[] = {
        { "id",                NULL }, /* patched below */
        { "version",           NULL },
        { "workbookVersionId", NULL },
        { "name",              NULL },
        { "description",       NULL },
    };
    uint16_t *offs[] = {
        &c->prog->id_off, &c->prog->version_off, &c->prog->workbook_version_id_off,
        &c->prog->name_off, &c->prog->description_off,
    };
    for (int i = 0; i < (int)(sizeof(k_provenance) / sizeof(k_provenance[0])); i++) {
        const sched_node_t *v = map_get(root, k_provenance[i].key);
        if (v == NULL) continue; /* all provenance keys are optional */
        /* string scalars only: .str is kept for every scalar kind, so a
         * non-NULL node_text says nothing — id: 123 / version: true /
         * workbookVersionId: 30m must not compile */
        if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_STR) {
            cerr(c, v, "header key '%s' must be a string", k_provenance[i].key);
            return false;
        }
        *offs[i] = pool_add(c, v, v->u.s.str);
        if (c->failed) return false;
    }
    /* optional macro list (gathered here so the strict loop below can treat
     * 'macros' as a known header key) */
    const sched_node_t *macros = map_get(root, "macros");
    if (macros != NULL && !compile_macros(c, macros)) return false;
    /* gather the two body keys; reject everything else (strict) */
    for (int i = 0; i < root->u.m.count; i++) {
        const char *key = root->u.m.pairs[i].key;
        bool header_key = strcmp(key, "schema") == 0 || strcmp(key, "macros") == 0;
        for (size_t k = 0; k < sizeof(k_provenance) / sizeof(k_provenance[0]); k++) {
            header_key = header_key || strcmp(key, k_provenance[k].key) == 0;
        }
        if (header_key) continue;
        if (strcmp(key, "protocols") == 0) { c->protocols_node = root->u.m.pairs[i].value; continue; }
        if (strcmp(key, "jobs") == 0)      { c->jobs_node = root->u.m.pairs[i].value; continue; }
        cerr(c, root->u.m.pairs[i].value, "unknown top-level key '%s'", key);
        return false;
    }
    return true;
}

/* ── protocols ───────────────────────────────────────────────────────── */

int64_t sched_estimate_ms(const sched_protocol_t *proto)
{
    int64_t ms = 0;
    for (int i = 0; i < proto->segment_count; i++) {
        ms += (int64_t)proto->segments[i].pulses * 1000 / proto->segments[i].freq
              + 300; /* per-segment configuration/light-sleep slack */
    }
    return ms;
}

static bool compile_segment(ctx_t *c, const sched_node_t *node, sched_segment_t *seg)
{
    if (node->kind != SCHED_NODE_MAP) {
        cerr(c, node, "protocol segment must be a mapping "
             "({pulses, freq, actinic, …})");
        return false;
    }
    /* Defaults cover all six AMBIT fields; shorthand is not the contract. */
    seg->type = 2;
    seg->far_red = 0;
    seg->subsampling = 1;
    seg->pulses = 0;
    seg->freq = 0;
    seg->actinic = 0;
    bool have_pulses = false, have_freq = false, have_actinic = false;
    for (int i = 0; i < node->u.m.count; i++) {
        const char *key = node->u.m.pairs[i].key;
        const sched_node_t *v = node->u.m.pairs[i].value;
        if (strcmp(key, "pulses") == 0 || strcmp(key, "freq") == 0) {
            if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_INT ||
                v->u.s.i < 1 || v->u.s.i > 65535) {
                cerr(c, v, "'%s' must be an integer 1..65535", key);
                return false;
            }
            if (key[0] == 'p') { seg->pulses = (uint16_t)v->u.s.i; have_pulses = true; }
            else               { seg->freq   = (uint16_t)v->u.s.i; have_freq = true; }
        } else if (strcmp(key, "actinic") == 0) {
            if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_INT ||
                v->u.s.i < -32768 || v->u.s.i > 32767) {
                cerr(c, v, "'actinic' must be an int16 "
                     "(-255..-1 raw DAC, 0 off, 1..9999 PAR µmol)");
                return false;
            }
            seg->actinic = (int16_t)v->u.s.i;
            have_actinic = true;
        } else if (strcmp(key, "type") == 0) {
            if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_INT ||
                v->u.s.i < 0 || v->u.s.i > 2) {
                cerr(c, v, "segment 'type' must be 0 (skip), 1 (incl. 730 nm) or 2 (no IR)");
                return false;
            }
            seg->type = (uint8_t)v->u.s.i;
        } else if (strcmp(key, "far_red") == 0) {
            if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_BOOL) {
                cerr(c, v, "'far_red' must be true/false");
                return false;
            }
            seg->far_red = (uint8_t)v->u.s.b;
        } else if (strcmp(key, "subsampling") == 0) {
            if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_INT ||
                v->u.s.i < 0 || v->u.s.i > 2) {
                cerr(c, v, "'subsampling' must be 0 (none), 1 (every pulse) or 2 (every 8 averaged)");
                return false;
            }
            seg->subsampling = (uint8_t)v->u.s.i;
        } else {
            cerr(c, v, "unknown segment key '%s'", key);
            return false;
        }
    }
    if (!have_pulses || !have_freq || !have_actinic) {
        cerr(c, node, "segment requires pulses, freq and actinic");
        return false;
    }
    if (seg->far_red && seg->type != 1) {
        cerr(c, node, "far_red: true is only meaningful with type: 1");
        return false;
    }
    return true;
}

static bool compile_protocols(ctx_t *c)
{
    const sched_node_t *node = c->protocols_node;
    if (node == NULL) return true;
    if (node->kind != SCHED_NODE_MAP) {
        cerr(c, node, "'protocols' must be a mapping of name → segment list");
        return false;
    }
    for (int i = 0; i < node->u.m.count; i++) {
        if (c->prog->protocol_count >= SCHED_SPEC_MAX_PROTOCOLS) {
            cerr(c, node->u.m.pairs[i].value, "protocol cap (%d)", SCHED_SPEC_MAX_PROTOCOLS);
            return false;
        }
        sched_protocol_t *proto = &c->prog->protocols[c->prog->protocol_count];
        memset(proto->segments, 0, sizeof(proto->segments));
        proto->persist = 0;
        proto->allow_interrupt = 0;
        proto->name_off = pool_add(c, node->u.m.pairs[i].value, node->u.m.pairs[i].key);
        if (c->failed) return false;
        const sched_node_t *v = node->u.m.pairs[i].value;
        const sched_node_t *seg_list = v;
        if (v->kind == SCHED_NODE_MAP) {
            /* run-level form: {segments: [...], persist: …, allow_interrupt: …} */
            seg_list = NULL;
            for (int k = 0; k < v->u.m.count; k++) {
                const char *key = v->u.m.pairs[k].key;
                const sched_node_t *kv = v->u.m.pairs[k].value;
                if (strcmp(key, "segments") == 0) { seg_list = kv; continue; }
                if (strcmp(key, "persist") == 0 || strcmp(key, "allow_interrupt") == 0) {
                    if (kv->kind != SCHED_NODE_SCALAR || kv->scal_kind != SCHED_SCAL_BOOL) {
                        cerr(c, kv, "'%s' must be true/false", key);
                        return false;
                    }
                    if (key[0] == 'p') proto->persist = (uint8_t)kv->u.s.b;
                    else               proto->allow_interrupt = (uint8_t)kv->u.s.b;
                    continue;
                }
                cerr(c, kv, "unknown protocol key '%s' (want segments/persist/allow_interrupt)", key);
                return false;
            }
            if (seg_list == NULL) {
                cerr(c, v, "protocol '%s' has no segments", node->u.m.pairs[i].key);
                return false;
            }
        }
        if (seg_list->kind != SCHED_NODE_SEQ || seg_list->u.q.count < 1) {
            cerr(c, seg_list, "protocol '%s' must be a non-empty segment list",
                 node->u.m.pairs[i].key);
            return false;
        }
        if (seg_list->u.q.count > SCHED_SPEC_MAX_SEGMENTS) {
            cerr(c, seg_list, "protocol '%s' has %d segments; AMBIT run arrays accept at most %d",
                 node->u.m.pairs[i].key, seg_list->u.q.count, SCHED_SPEC_MAX_SEGMENTS);
            return false;
        }
        for (int s = 0; s < seg_list->u.q.count; s++) {
            if (!compile_segment(c, seg_list->u.q.items[s], &proto->segments[s])) return false;
        }
        proto->segment_count = (uint8_t)seg_list->u.q.count;
        c->prog->protocol_count++;
    }
    return true;
}

static const sched_protocol_t *find_protocol(const sched_program_t *p, const char *name)
{
    for (int i = 0; i < p->protocol_count; i++) {
        const char *pn = sched_pool_str(p, p->protocols[i].name_off);
        if (pn != NULL && strcmp(pn, name) == 0) return &p->protocols[i];
    }
    return NULL;
}

/* An ambit/trace step's protocol name (NULL if the required input is absent)
 * and, when margin != NULL, its deadline_margin (default materialized by
 * compile_with, so this normally just reads the entry). */
static const char *trace_protocol(const sched_program_t *prog,
                                  const sched_step_t *step, int64_t *margin)
{
    const char *pname = NULL;
    if (margin != NULL) *margin = TRACE_MARGIN_DEFAULT_MS;
    for (int e = 0; e < step->entry_count; e++) {
        const sched_entry_t *en = &prog->entries[step->entry_start + e];
        const char *iname = step->action->inputs[en->input_idx].name;
        if (strcmp(iname, "protocol") == 0 && en->type == SCHED_VAL_STR) {
            pname = sched_pool_str(prog, en->u.str_off);
        }
        if (margin != NULL && strcmp(iname, "deadline_margin") == 0) {
            *margin = en->u.i;
        }
    }
    return pname;
}

/* ── triggers ────────────────────────────────────────────────────────── */

/* Signed duration: duration scalar, or a string "+30m"/"-15m"/"1h". The
 * magnitude/unit math is the shared tri-state core (sched_internal.h), so
 * this path can never diverge from the YAML lexer's overflow behaviour:
 * a sign with no digits is a grammar error, and out-of-range magnitudes
 * reject as "duration too large" before any multiply. */
static bool parse_signed_duration(ctx_t *c, const sched_node_t *n, int64_t *out_ms)
{
    if (n->kind == SCHED_NODE_SCALAR && n->scal_kind == SCHED_SCAL_DURATION_MS) {
        *out_ms = n->u.s.ms;
        return true;
    }
    if (n->kind == SCHED_NODE_SCALAR && n->scal_kind == SCHED_SCAL_STR) {
        const char *s = n->u.s.str;
        bool neg = false;
        if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
        int64_t v;
        int rd = sched_duration_ms(s, strlen(s), &v);
        if (rd == 1) {
            *out_ms = neg ? -v : v;
            return true;
        }
        if (rd < 0) {
            cerr(c, n, "duration too large");
            return false;
        }
    }
    cerr(c, n, "expected a duration (e.g. 30m, -15m)");
    return false;
}

/* Exact clock grammar for quoted strings: 1–2 hour digits, ':', exactly 2
 * minute digits, full consumption. Used by window edges; sscanf's %2d:%2d
 * would accept partial matches like "1:2x".
 * Mirrors the plain-scalar HHMM lexer in sched_yaml.c. */
static bool parse_clock_str(const char *s, int *hh, int *mm)
{
    int i = 0, h, m;
    if (s[0] < '0' || s[0] > '9') return false;
    h = s[i++] - '0';
    if (s[i] >= '0' && s[i] <= '9') h = h * 10 + (s[i++] - '0');
    if (s[i] != ':') return false;
    i++;
    if (s[i] < '0' || s[i] > '9') return false;  /* minutes need both digits */
    m = s[i++] - '0';
    if (s[i] < '0' || s[i] > '9') return false;
    m = m * 10 + (s[i++] - '0');
    if (s[i] != '\0') return false;              /* suffix or second colon */
    if (h > 23 || m > 59) return false;
    *hh = h;
    *mm = m;
    return true;
}

/* Identify a cron whose within-hour occurrences form a regular cadence and
 * whose larger calendar fields are wildcards. Seconds-first expressions can
 * then use the monotonic interval machinery; five-field expressions retain
 * wall/DST semantics but still expose their period to the protocol fit check.
 * Irregular and calendar-constrained expressions remain wall cron. */
static bool cron_fixed_cadence(const sched_cron_t *cron,
                               int64_t *period_ms, int64_t *phase_ms)
{
    if (cron->hour != ((UINT32_C(1) << 24) - 1) ||
        cron->dom != UINT32_C(0xFFFFFFFE) || cron->month != UINT16_C(0x1FFE) ||
        cron->dow != UINT8_C(0x7F)) {
        return false;
    }

    int first = -1, previous = -1, period = -1;
    for (int minute = 0; minute < 60; minute++) {
        if ((cron->min & (UINT64_C(1) << minute)) == 0) continue;
        for (int second = 0; second < 60; second++) {
            if ((cron->sec & (UINT64_C(1) << second)) == 0) continue;
            int instant = minute * 60 + second;
            if (first < 0) first = instant;
            if (previous >= 0) {
                int gap = instant - previous;
                if (period < 0) period = gap;
                else if (gap != period) return false;
            }
            previous = instant;
        }
    }
    if (first < 0) return false;
    int wrap_gap = 3600 + first - previous;
    if (period < 0) period = wrap_gap;
    if (wrap_gap != period) return false;
    *period_ms = (int64_t)period * 1000;
    *phase_ms = (int64_t)(first % period) * 1000;
    return true;
}

static bool compile_trigger(ctx_t *c, const sched_node_t *node, sched_trigger_t *t)
{
    memset(t, 0, sizeof(*t));
    if (node->kind == SCHED_NODE_SCALAR) {
        const char *s = node_text(node);
        if (s != NULL && strcmp(s, "boot") == 0 && node->scal_kind == SCHED_SCAL_STR) {
            t->kind = SCHED_TRIG_BOOT;
            return true;
        }
        if (s != NULL && strcmp(s, "dispatch") == 0 && node->scal_kind == SCHED_SCAL_STR) {
            t->kind = SCHED_TRIG_DISPATCH;
            return true;
        }
        cerr(c, node, "schedule entry must be 'boot', 'dispatch', or a mapping "
             "with cron or solar");
        return false;
    }
    if (node->kind != SCHED_NODE_MAP) {
        cerr(c, node, "schedule entry must be a mapping with cron or solar");
        return false;
    }
    /* Cron is the normal schedule. Solar is the one explicit extension for
     * location-dependent events that no cron expression can know. */
    const sched_node_t *cron     = map_get(node, "cron");
    const sched_node_t *solar    = map_get(node, "solar");
    const sched_node_t *offset = map_get(node, "offset");
    for (int i = 0; i < node->u.m.count; i++) {
        const char *key = node->u.m.pairs[i].key;
        bool known = strcmp(key, "cron") == 0 || strcmp(key, "solar") == 0 ||
                     strcmp(key, "offset") == 0;
        if (!known) {
            cerr(c, node->u.m.pairs[i].value, "unknown schedule key '%s'", key);
            return false;
        }
    }
    int count = (cron != NULL) + (solar != NULL);
    if (count == 0) {
        cerr(c, node, "schedule mapping needs one of cron/solar");
        return false;
    }
    if (count > 1) {
        cerr(c, node, "schedule mapping has %d schedule keys; exactly one of "
             "cron/solar is allowed", count);
        return false;
    }
    if (cron != NULL) {
        const char *expr = node_text(cron);
        if (expr == NULL) {
            cerr(c, cron, "'cron' must be a string with 5 fields, or 6 with seconds");
            return false;
        }
        char cerr_buf[128];
        sched_cron_t parsed;
        if (sched_cron_parse(expr, &parsed, cerr_buf, sizeof(cerr_buf)) != ESP_OK) {
            cerr(c, cron, "%s", cerr_buf);
            return false;
        }
        if (offset != NULL) {
            cerr(c, offset, "'offset' only applies to solar schedules");
            return false;
        }
        int64_t period_ms, phase_ms;
        if (parsed.has_seconds &&
            cron_fixed_cadence(&parsed, &period_ms, &phase_ms)) {
            t->kind = SCHED_TRIG_INTERVAL;
            t->u.interval.period_ms = period_ms;
            t->u.interval.phase_ms = phase_ms;
        } else {
            t->kind = SCHED_TRIG_CRON;
            t->u.cron = parsed;
        }
        return true;
    }
    /* solar */
    t->kind = SCHED_TRIG_SOLAR;
    const char *ev = node_text(solar);
    if (ev == NULL ||
        (strcmp(ev, "sunrise") != 0 && strcmp(ev, "sunset") != 0)) {
        cerr(c, solar, "'solar' must be sunrise or sunset");
        return false;
    }
    t->u.solar.event = strcmp(ev, "sunrise") == 0 ? TIME_SYNC_SUNRISE : TIME_SYNC_SUNSET;
    t->u.solar.offset_s = 0;
    if (offset != NULL) {
        int64_t ms;
        if (!parse_signed_duration(c, offset, &ms)) return false;
        if (ms % 1000 != 0 || ms / 1000 > SOLAR_OFFSET_MAX_S || ms / 1000 < -SOLAR_OFFSET_MAX_S) {
            cerr(c, offset, "solar 'offset' must be a whole-second duration within ±12h");
            return false;
        }
        t->u.solar.offset_s = (int32_t)(ms / 1000);
    }
    return true;
}

/* ── window / gate ───────────────────────────────────────────────────── */

/* sun-expr: sunrise|sunset[±dur] */
static bool parse_edge(ctx_t *c, const sched_node_t *n, sched_edge_t *e)
{
    memset(e, 0, sizeof(*e));
    if (n->kind == SCHED_NODE_SCALAR && n->scal_kind == SCHED_SCAL_HHMM) {
        e->kind = SCHED_EDGE_CLOCK;
        e->hh = (uint8_t)n->u.s.hh;
        e->mm = (uint8_t)n->u.s.mm;
        return true;
    }
    const char *s = node_text(n);
    if (n->kind != SCHED_NODE_SCALAR || n->scal_kind != SCHED_SCAL_STR || s == NULL) {
        cerr(c, n, "window edge must be HH:MM or sunrise|sunset[±dur]");
        return false;
    }
    { /* a quoted "10:00" arrives as STR, not HHMM — accept the exact grammar */
        int h = 0, m = 0;
        if (parse_clock_str(s, &h, &m)) {
            e->kind = SCHED_EDGE_CLOCK;
            e->hh = (uint8_t)h;
            e->mm = (uint8_t)m;
            return true;
        }
    }
    int event;
    size_t base;
    if (strncmp(s, "sunrise", 7) == 0) { event = TIME_SYNC_SUNRISE; base = 7; }
    else if (strncmp(s, "sunset", 6) == 0) { event = TIME_SYNC_SUNSET; base = 6; }
    else {
        cerr(c, n, "window edge must be HH:MM or sunrise|sunset[±dur]");
        return false;
    }
    if (s[base] == '\0') {
        e->kind = SCHED_EDGE_SUN;
        e->event = (uint8_t)event;
        return true;
    }
    if (s[base] != '+' && s[base] != '-') {
        cerr(c, n, "window edge must be HH:MM or sunrise|sunset[±dur]");
        return false;
    }
    sched_node_t fake = *n; /* parse_signed_duration reports at this node */
    int64_t ms;
    fake.u.s.str = s + base;
    if (!parse_signed_duration(c, &fake, &ms)) return false;
    if (ms % 1000 != 0 || ms / 1000 > SOLAR_OFFSET_MAX_S || ms / 1000 < -SOLAR_OFFSET_MAX_S) {
        cerr(c, n, "sun edge offset must be a whole-second duration within ±12h");
        return false;
    }
    e->kind = SCHED_EDGE_SUN;
    e->event = (uint8_t)event;
    e->offset_s = (int32_t)(ms / 1000);
    return true;
}

static bool edges_equal(const sched_edge_t *a, const sched_edge_t *b)
{
    return a->kind == b->kind && a->event == b->event && a->hh == b->hh &&
           a->mm == b->mm && a->offset_s == b->offset_s;
}

static bool compile_when(ctx_t *c, const sched_node_t *when, sched_job_t *job)
{
    if (when->kind != SCHED_NODE_MAP) {
        cerr(c, when, "'when' must be a block mapping containing 'window'");
        return false;
    }
    const sched_node_t *win = map_get(when, "window");
    for (int i = 0; i < when->u.m.count; i++) {
        if (strcmp(when->u.m.pairs[i].key, "window") != 0) {
            cerr(c, when->u.m.pairs[i].value, "unknown 'when' key '%s'",
                 when->u.m.pairs[i].key);
            return false;
        }
    }
    if (win == NULL) {
        cerr(c, when, "'when' currently supports only 'window'");
        return false;
    }
    sched_window_t *w = &job->window;
    memset(w, 0, sizeof(*w));
    w->unresolved = SCHED_UNRESOLVED_SKIP; /* every window carries the policy; default skip */
    const char *word = node_text(win);
    if (win->kind == SCHED_NODE_SCALAR && word != NULL &&
        (strcmp(word, "day") == 0 || strcmp(word, "night") == 0)) {
        /* day/night lower to sunrise..sunset and its complement; the hint
         * keeps is_daytime's polar fallback (design §Gates) */
        bool day = strcmp(word, "day") == 0;
        w->hint = day ? SCHED_WIN_DAY : SCHED_WIN_NIGHT;
        w->from.kind = SCHED_EDGE_SUN;
        w->from.event = day ? TIME_SYNC_SUNRISE : TIME_SYNC_SUNSET;
        w->to.kind = SCHED_EDGE_SUN;
        w->to.event = day ? TIME_SYNC_SUNSET : TIME_SYNC_SUNRISE;
        job->has_window = 1;
        return true;
    }
    if (win->kind != SCHED_NODE_MAP) {
        cerr(c, win, "'window' must be day, night, or a from/to block mapping");
        return false;
    }
    w->hint = SCHED_WIN_EXPLICIT;
    const sched_node_t *from = map_get(win, "from");
    const sched_node_t *to   = map_get(win, "to");
    const sched_node_t *unr  = map_get(win, "unresolved");
    for (int i = 0; i < win->u.m.count; i++) {
        const char *key = win->u.m.pairs[i].key;
        if (strcmp(key, "from") != 0 && strcmp(key, "to") != 0 &&
            strcmp(key, "unresolved") != 0) {
            cerr(c, win->u.m.pairs[i].value, "unknown window key '%s'", key);
            return false;
        }
    }
    if (from == NULL || to == NULL) {
        cerr(c, win, "'window' needs both from: and to:");
        return false;
    }
    if (!parse_edge(c, from, &w->from)) return false;
    if (!parse_edge(c, to, &w->to)) return false;
    if (edges_equal(&w->from, &w->to)) {
        cerr(c, to, "window from == to is not a window");
        return false;
    }
    if (unr != NULL) {
        const char *u = node_text(unr);
        if (u == NULL || (strcmp(u, "run") != 0 && strcmp(u, "skip") != 0)) {
            cerr(c, unr, "'unresolved' must be run or skip");
            return false;
        }
        w->unresolved = strcmp(u, "run") == 0 ? SCHED_UNRESOLVED_RUN : SCHED_UNRESOLVED_SKIP;
    }
    job->has_window = 1;
    return true;
}

/* ── steps / with ────────────────────────────────────────────────────── */

static bool fill_entry(ctx_t *c, const sched_node_t *v,
                       const sched_input_decl_t *decl, uint8_t input_idx,
                       sched_entry_t *e)
{
    memset(e, 0, sizeof(*e));
    e->input_idx = input_idx;
    e->key_off = SCHED_POOL_NONE;
    if (v == NULL) {
        /* materialize the declared default so the runtime sees a complete
         * input set without re-knowing the table */
        switch (decl->type) {
        case SCHED_IN_BOOL:  e->type = SCHED_VAL_BOOL; e->u.i = decl->def_i; return true;
        case SCHED_IN_INT:
        case SCHED_IN_DURATION_MS:
            e->type = SCHED_VAL_INT; e->u.i = decl->def_i; return true;
        case SCHED_IN_STRING:
            if (decl->def_s == NULL) return true; /* no default: absent */
            e->type = SCHED_VAL_STR;
            e->u.str_off = pool_add(c, NULL, decl->def_s);
            return !c->failed;
        case SCHED_IN_CHANNELS:
            /* absent = "all channels that answer a ping". n == 0 can only
             * arise here (explicit lists must be non-empty), so the runner
             * can tell "all present" from "these n". */
            e->type = SCHED_VAL_CHANNELS; /* u.chans.n stays 0 */
            return true;
        default:
            return true; /* MAP has no default */
        }
    }
    switch (decl->type) {
    case SCHED_IN_INT:
        if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_INT) {
            cerr(c, v, "input '%s' must be an integer", decl->name);
            return false;
        }
        if ((decl->min != 0 || decl->max != 0) &&
            (v->u.s.i < decl->min || v->u.s.i > decl->max)) {
            cerr(c, v, "input '%s' must be in %lld..%lld", decl->name,
                 (long long)decl->min, (long long)decl->max);
            return false;
        }
        e->type = SCHED_VAL_INT;
        e->u.i = v->u.s.i;
        return true;
    case SCHED_IN_FLOAT:
        if (v->kind != SCHED_NODE_SCALAR ||
            (v->scal_kind != SCHED_SCAL_FLOAT && v->scal_kind != SCHED_SCAL_INT)) {
            cerr(c, v, "input '%s' must be a number", decl->name);
            return false;
        }
        e->type = SCHED_VAL_FLOAT;
        e->u.f = v->scal_kind == SCHED_SCAL_FLOAT ? v->u.s.f : (double)v->u.s.i;
        return true;
    case SCHED_IN_BOOL:
        if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_BOOL) {
            cerr(c, v, "input '%s' must be true/false", decl->name);
            return false;
        }
        e->type = SCHED_VAL_BOOL;
        e->u.i = v->u.s.b;
        return true;
    case SCHED_IN_STRING: {
        const char *s = node_text(v);
        if (v->kind != SCHED_NODE_SCALAR || s == NULL ||
            (v->scal_kind != SCHED_SCAL_STR && v->scal_kind != SCHED_SCAL_HHMM)) {
            cerr(c, v, "input '%s' must be a string", decl->name);
            return false;
        }
        e->type = SCHED_VAL_STR;
        e->u.str_off = pool_add(c, v, s);
        return !c->failed;
    }
    case SCHED_IN_DURATION_MS:
        if (v->kind != SCHED_NODE_SCALAR || v->scal_kind != SCHED_SCAL_DURATION_MS) {
            cerr(c, v, "input '%s' must be a duration (e.g. 15s)", decl->name);
            return false;
        }
        if (v->u.s.ms < decl->min || (decl->max > 0 && v->u.s.ms > decl->max)) {
            cerr(c, v, "input '%s' must be in %lld..%lld ms", decl->name,
                 (long long)decl->min, (long long)decl->max);
            return false;
        }
        e->type = SCHED_VAL_INT; /* durations materialize as integer ms */
        e->u.i = v->u.s.ms;
        return true;
    case SCHED_IN_CHANNELS: {
        if (v->kind != SCHED_NODE_SEQ || v->u.q.count < 1) {
            cerr(c, v, "input '%s' must be a non-empty block list of channels",
                 decl->name);
            return false;
        }
        if (v->u.q.count > SCHED_SPEC_MAX_CHANNELS) {
            cerr(c, v, "input '%s': at most %d channels", decl->name, SCHED_SPEC_MAX_CHANNELS);
            return false;
        }
        e->type = SCHED_VAL_CHANNELS;
        uint8_t seen = 0;
        for (int i = 0; i < v->u.q.count; i++) {
            const sched_node_t *ch = v->u.q.items[i];
            if (ch->kind != SCHED_NODE_SCALAR || ch->scal_kind != SCHED_SCAL_INT ||
                ch->u.s.i < 0 || ch->u.s.i >= SCHED_SPEC_MAX_CHANNELS) {
                cerr(c, ch, "channels are 0..%d", SCHED_SPEC_MAX_CHANNELS - 1);
                return false;
            }
            if (seen & (1u << ch->u.s.i)) {
                cerr(c, ch, "duplicate channel %lld", (long long)ch->u.s.i);
                return false;
            }
            seen |= (uint8_t)(1u << ch->u.s.i);
            e->u.chans.v[e->u.chans.n++] = (uint8_t)ch->u.s.i;
        }
        return true;
    }
    case SCHED_IN_MAP: {
        if (v->kind != SCHED_NODE_MAP) {
            cerr(c, v, "input '%s' must be a flat mapping", decl->name);
            return false;
        }
        if (v->u.m.count > SCHED_SPEC_MAX_EVENT_KEYS) {
            cerr(c, v, "input '%s' has %d keys; the cap is %d",
                 decl->name, v->u.m.count, SCHED_SPEC_MAX_EVENT_KEYS);
            return false;
        }
        /* map entries are appended by the caller loop; signal via type */
        e->type = SCHED_VAL_STR; /* unused for the map itself */
        return true;
    }
    }
    cerr(c, v, "unsupported input type");
    return false;
}

static bool compile_with(ctx_t *c, const sched_node_t *with,
                         const sched_action_t *action, sched_step_t *step)
{
    step->entry_start = c->prog->entry_count;
    step->entry_count = 0;
    bool present[SCHED_SPEC_MAX_INPUTS] = { false };

    if (with != NULL) {
        if (with->kind != SCHED_NODE_MAP) {
            cerr(c, with, "'with' must be a mapping of input → value");
            return false;
        }
        for (int i = 0; i < with->u.m.count; i++) {
            const char *key = with->u.m.pairs[i].key;
            const sched_node_t *v = with->u.m.pairs[i].value;
            int idx = -1;
            for (uint8_t k = 0; k < action->input_count; k++) {
                if (strcmp(action->inputs[k].name, key) == 0) { idx = k; break; }
            }
            if (idx < 0) {
                cerr(c, v, "unknown input '%s' for action %s", key, action->name);
                return false;
            }
            const sched_input_decl_t *decl = &action->inputs[idx];
            if (decl->type == SCHED_IN_MAP) {
                if (v->kind != SCHED_NODE_MAP || v->u.m.count > SCHED_SPEC_MAX_EVENT_KEYS) {
                    cerr(c, v, "input '%s' must be a flat mapping of at most %d keys",
                         decl->name, SCHED_SPEC_MAX_EVENT_KEYS);
                    return false;
                }
                for (int k = 0; k < v->u.m.count; k++) {
                    const sched_node_t *mv = v->u.m.pairs[k].value;
                    if (mv->kind != SCHED_NODE_SCALAR) {
                        cerr(c, mv, "input '%s' values must be scalars", decl->name);
                        return false;
                    }
                    sched_entry_t *e = entry_add(c, mv);
                    if (e == NULL) return false;
                    memset(e, 0, sizeof(*e));
                    e->input_idx = (uint8_t)idx;
                    e->key_off = pool_add(c, mv, v->u.m.pairs[k].key);
                    if (c->failed) return false;
                    switch (mv->scal_kind) {
                    case SCHED_SCAL_INT:  e->type = SCHED_VAL_INT; e->u.i = mv->u.s.i; break;
                    case SCHED_SCAL_FLOAT: e->type = SCHED_VAL_FLOAT; e->u.f = mv->u.s.f; break;
                    case SCHED_SCAL_BOOL: e->type = SCHED_VAL_BOOL; e->u.i = mv->u.s.b; break;
                    case SCHED_SCAL_DURATION_MS:
                        e->type = SCHED_VAL_INT; e->u.i = mv->u.s.ms; break;
                    default: {
                        const char *s = node_text(mv);
                        if (s != NULL && s[0] == '$' && !sched_is_placeholder(s)) {
                            cerr(c, mv, "unknown placeholder '%s'", s);
                            return false;
                        }
                        e->type = SCHED_VAL_STR;
                        e->u.str_off = pool_add(c, mv, s);
                        if (c->failed) return false;
                        break;
                    }
                    }
                    step->entry_count++;
                }
                present[idx] = true;
                continue;
            }
            sched_entry_t *e = entry_add(c, v);
            if (e == NULL) return false;
            if (!fill_entry(c, v, decl, (uint8_t)idx, e)) return false;
            step->entry_count++;
            present[idx] = true;
        }
    }
    /* required inputs present; defaults materialized */
    for (uint8_t k = 0; k < action->input_count; k++) {
        const sched_input_decl_t *decl = &action->inputs[k];
        if (present[k]) continue;
        if (decl->required) {
            cerr(c, with, "action %s requires input '%s'", action->name, decl->name);
            return false;
        }
        if (!decl->has_default) continue;
        sched_entry_t *e = entry_add(c, with);
        if (e == NULL) return false;
        uint16_t before = step->entry_count;
        if (!fill_entry(c, NULL, decl, k, e)) return false;
        /* STRING with def_s == NULL leaves an empty entry; drop it */
        if (decl->type == SCHED_IN_STRING && decl->def_s == NULL) {
            c->prog->entry_count--;
        } else {
            step->entry_count = (uint8_t)(before + 1);
        }
    }
    return true;
}

/* ── jobs ────────────────────────────────────────────────────────────── */

static bool compile_enum(ctx_t *c, const sched_node_t *n, const char *key,
                         const char *const *words, int n_words, uint8_t *out)
{
    const char *s = node_text(n);
    if (n->kind != SCHED_NODE_SCALAR || s == NULL) {
        goto bad;
    }
    for (int i = 0; i < n_words; i++) {
        if (strcmp(s, words[i]) == 0) {
            *out = (uint8_t)i;
            return true;
        }
    }
bad:;
    /* build "a|b|c" inline */
    char opts[64] = { 0 };
    for (int i = 0; i < n_words; i++) {
        strncat(opts, words[i], sizeof(opts) - strlen(opts) - 2);
        if (i + 1 < n_words) strncat(opts, "|", sizeof(opts) - strlen(opts) - 1);
    }
    cerr(c, n, "'%s' must be one of %s", key, opts);
    return false;
}

static bool compile_job(ctx_t *c, const char *name, const sched_node_t *node,
                        sched_job_t *job)
{
    if (node->kind != SCHED_NODE_MAP) {
        cerr(c, node, "job '%s' must be a mapping", name);
        return false;
    }
    memset(job->triggers, 0, sizeof(job->triggers));
    memset(job->steps, 0, sizeof(job->steps));
    job->trigger_count = 0;
    job->step_count = 0;
    job->overlap = SCHED_OVERLAP_SKIP;
    job->missed = SCHED_MISSED_SKIP;
    job->on_enter = 0;
    job->has_window = 0;
    bool on_enter_set = false;

    const sched_node_t *schedule = map_get(node, "schedule");
    const sched_node_t *when  = map_get(node, "when");
    const sched_node_t *steps = map_get(node, "steps");
    static const char *const k_job_keys[] = {
        "schedule", "when", "overlap", "missed", "on_enter", "steps",
    };
    for (int i = 0; i < node->u.m.count; i++) {
        const char *key = node->u.m.pairs[i].key;
        bool known = false;
        for (size_t k = 0; k < sizeof(k_job_keys) / sizeof(k_job_keys[0]); k++) {
            known = known || strcmp(key, k_job_keys[k]) == 0;
        }
        if (!known) {
            cerr(c, node->u.m.pairs[i].value, "unknown job key '%s'", key);
            return false;
        }
    }
    if (schedule == NULL) {
        cerr(c, node, "job '%s' has no 'schedule' (use a boot/dispatch entry when needed)", name);
        return false;
    }
    if (steps == NULL) {
        cerr(c, node, "job '%s' has no 'steps'", name);
        return false;
    }

    /* overlap / missed (JII standard deployment-schedule enums) */
    const sched_node_t *ov = map_get(node, "overlap");
    if (ov != NULL) {
        static const char *const k_overlap[] = { "skip", "queue-one", "reject" };
        if (!compile_enum(c, ov, "overlap", k_overlap, 3, &job->overlap)) return false;
    }
    const sched_node_t *mi = map_get(node, "missed");
    if (mi != NULL) {
        static const char *const k_missed[] = { "skip", "run-once" };
        if (!compile_enum(c, mi, "missed", k_missed, 2, &job->missed)) return false;
    }

    /* gate */
    if (when != NULL && !compile_when(c, when, job)) return false;

    const sched_node_t *oe = map_get(node, "on_enter");
    if (oe != NULL) {
        if (oe->kind != SCHED_NODE_SCALAR || oe->scal_kind != SCHED_SCAL_BOOL) {
            cerr(c, oe, "'on_enter' must be true/false");
            return false;
        }
        job->on_enter = (uint8_t)oe->u.s.b;
        on_enter_set = true;
    }
    /* gated jobs fire once on window entry by default (the legacy script's
     * "immediate sample on entering a phase"); ungated jobs have no gate to
     * enter, so the default is off */
    if (!on_enter_set) job->on_enter = job->has_window;

    /* triggers */
    if (schedule->kind == SCHED_NODE_SEQ) {
        if (schedule->u.q.count < 1 || schedule->u.q.count > SCHED_SPEC_MAX_TRIGGERS) {
            cerr(c, schedule, "job '%s' has %d schedule entries; the cap is %d",
                 name, schedule->u.q.count, SCHED_SPEC_MAX_TRIGGERS);
            return false;
        }
        for (int i = 0; i < schedule->u.q.count; i++) {
            if (!compile_trigger(c, schedule->u.q.items[i],
                                 &job->triggers[job->trigger_count])) {
                return false;
            }
            job->trigger_count++;
        }
    } else {
        if (!compile_trigger(c, schedule, &job->triggers[0])) return false;
        job->trigger_count = 1;
    }

    /* dispatch-only jobs with when: are pointless (a gate nobody waits behind) */
    bool all_dispatch = true;
    for (int i = 0; i < job->trigger_count; i++) {
        if (job->triggers[i].kind != SCHED_TRIG_DISPATCH) all_dispatch = false;
    }
    if (all_dispatch && job->has_window) {
        cerr(c, when, "dispatch-only job '%s' has a 'when' gate nobody waits behind",
             name);
        return false;
    }

    /* steps */
    if (steps->kind != SCHED_NODE_SEQ || steps->u.q.count < 1) {
        cerr(c, steps, "job '%s' needs a non-empty 'steps' list", name);
        return false;
    }
    if (steps->u.q.count > SCHED_SPEC_MAX_STEPS) {
        cerr(c, steps, "job '%s' has %d steps; the cap is %d",
             name, steps->u.q.count, SCHED_SPEC_MAX_STEPS);
        return false;
    }
    for (int i = 0; i < steps->u.q.count; i++) {
        const sched_node_t *sn = steps->u.q.items[i];
        if (sn->kind != SCHED_NODE_MAP) {
            cerr(c, sn, "step must be a block mapping containing 'uses'");
            return false;
        }
        const sched_node_t *uses = map_get(sn, "uses");
        const sched_node_t *with = map_get(sn, "with");
        const sched_node_t *coe  = map_get(sn, "continue-on-error");
        for (int k = 0; k < sn->u.m.count; k++) {
            const char *key = sn->u.m.pairs[k].key;
            if (strcmp(key, "uses") != 0 && strcmp(key, "with") != 0 &&
                strcmp(key, "continue-on-error") != 0) {
                cerr(c, sn->u.m.pairs[k].value, "unknown step key '%s'", key);
                return false;
            }
        }
        if (uses == NULL) {
            cerr(c, sn, "step has no 'uses'");
            return false;
        }
        const char *aname = node_text(uses);
        if (aname == NULL) {
            cerr(c, uses, "'uses' must be an action name");
            return false;
        }
        const sched_action_t *action = sched_action_find(aname);
        if (action == NULL) {
            cerr(c, uses, "unknown action '%s' (see sched_host --schema)", aname);
            return false;
        }
        sched_step_t *step = &job->steps[job->step_count];
        step->action = action;
        step->continue_on_error = 0;
        if (coe != NULL) {
            if (coe->kind != SCHED_NODE_SCALAR || coe->scal_kind != SCHED_SCAL_BOOL) {
                cerr(c, coe, "'continue-on-error' must be true/false");
                return false;
            }
            step->continue_on_error = (uint8_t)coe->u.s.b;
        }
        if (!compile_with(c, with, action, step)) return false;
        job->step_count++;
    }

    /* Resolve every ambit/trace protocol here regardless of trigger kind; a
     * boot, cron, or solar trace with a missing protocol must not reach
     * runtime. The duration-vs-period check below applies when regular cron
     * lowered to a fixed cadence. */
    for (int si = 0; si < job->step_count; si++) {
        const sched_step_t *step = &job->steps[si];
        if (strcmp(step->action->name, "ambit/trace") != 0) continue;
        const char *pname = trace_protocol(c->prog, step, NULL);
        if (pname == NULL) continue; /* required-input check already ran */
        if (find_protocol(c->prog, pname) == NULL) {
            cerr(c, node, "job '%s' references unknown protocol '%s'", name, pname);
            return false;
        }
    }

    /* duration vs period (the critique's highest-value check: the 59-pulse
     * SS on a 1 m grid would have been caught by this) */
    for (int ti = 0; ti < job->trigger_count; ti++) {
        const sched_trigger_t *t = &job->triggers[ti];
        int64_t period, unused_phase;
        if (t->kind == SCHED_TRIG_INTERVAL) {
            period = t->u.interval.period_ms;
        } else if (t->kind == SCHED_TRIG_CRON &&
                   cron_fixed_cadence(&t->u.cron, &period, &unused_phase)) {
            /* A five-field fixed grid keeps calendar/DST semantics, but its
             * period is still known well enough for the fit check. */
        } else {
            continue;
        }
        for (int si = 0; si < job->step_count; si++) {
            const sched_step_t *step = &job->steps[si];
            if (strcmp(step->action->name, "ambit/trace") != 0) continue;
            int64_t margin = TRACE_MARGIN_DEFAULT_MS;
            const char *pname = trace_protocol(c->prog, step, &margin);
            if (pname == NULL) continue; /* required-input check already ran */
            const sched_protocol_t *proto = find_protocol(c->prog, pname);
            /* Pulse-train time + deadline margin vs the grid period. The
             * check exists to catch a protocol whose worst case cannot fit
             * its grid, and the worst case is bounded by the deadline
             * margin, which exists precisely to absorb transport slack — of
             * which the estimator's 300 ms/segment (sched_estimate_ms) is a
             * small part. So the 300 ms is NOT added here; tuning the
             * science (fewer pulses in the shipped schedule) or the safety
             * margin to satisfy the check would be backwards. Net effect:
             * the shipped default fits exactly (SS 45 pulses: 45.0 s +
             * 15 s = 60.0 s on a 1 m grid) while the incident case, 59
             * pulses, still fails at 74 s. sched_estimate_ms keeps the
             * firmware-exact +300 ms/segment for the runner's poll
             * scheduling. (Orchestrator ruling on plan ambiguity, T2.) */
            int64_t pulse_ms = 0;
            for (int s = 0; s < proto->segment_count; s++) {
                pulse_ms += (int64_t)proto->segments[s].pulses * 1000 /
                            proto->segments[s].freq;
            }
            int64_t est = pulse_ms + margin;
            if (est > period) {
                cerr(c, node,
                     "job '%s': protocol '%s' needs ~%lld ms + %lld ms margin, "
                     "over the %lld ms period; the run would always start late",
                     name, pname, (long long)pulse_ms, (long long)margin,
                     (long long)period);
                return false;
            }
        }
    }
    return true;
}

static bool compile_jobs(ctx_t *c)
{
    const sched_node_t *node = c->jobs_node;
    if (node == NULL || node->kind != SCHED_NODE_MAP || node->u.m.count < 1) {
        cerr(c, node, "'jobs' must be a non-empty mapping of name → job");
        return false;
    }
    if (node->u.m.count > SCHED_SPEC_MAX_JOBS) {
        cerr(c, node, "%d jobs; the cap is %d", node->u.m.count, SCHED_SPEC_MAX_JOBS);
        return false;
    }
    /* the parser already rejected duplicate keys, so job names are unique */
    for (int i = 0; i < node->u.m.count; i++) {
        sched_job_t *job = &c->prog->jobs[c->prog->job_count];
        job->name_off = pool_add(c, node->u.m.pairs[i].value, node->u.m.pairs[i].key);
        if (c->failed) return false;
        if (!compile_job(c, node->u.m.pairs[i].key, node->u.m.pairs[i].value, job)) {
            return false;
        }
        c->prog->job_count++;
    }
    return true;
}

/* ── entry points ────────────────────────────────────────────────────── */

const sched_job_t *sched_find_job(const sched_program_t *p, const char *name)
{
    if (p == NULL || name == NULL) return NULL;
    for (int i = 0; i < p->job_count; i++) {
        const char *jn = sched_pool_str(p, p->jobs[i].name_off);
        if (jn != NULL && strcmp(jn, name) == 0) return &p->jobs[i];
    }
    return NULL;
}

esp_err_t sched_compile(const sched_node_t *root, sched_program_t *out,
                        char *err, size_t err_cap)
{
    if (root == NULL || out == NULL || err == NULL || err_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->id_off = SCHED_POOL_NONE;
    out->version_off = SCHED_POOL_NONE;
    out->workbook_version_id_off = SCHED_POOL_NONE;
    out->name_off = SCHED_POOL_NONE;
    out->description_off = SCHED_POOL_NONE;

    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.prog = out;
    c.err = err;
    c.err_cap = err_cap;

    if (!compile_header(&c, root)) return ESP_FAIL;
    if (!compile_protocols(&c)) return ESP_FAIL;
    if (!compile_jobs(&c)) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t sched_compile_text(const char *text, size_t len, sched_program_t *out,
                             char *err, size_t err_cap)
{
    sched_yaml_doc_t *doc = NULL;
    esp_err_t rc = sched_yaml_parse(text, len, &doc, err, err_cap);
    if (rc != ESP_OK) return rc;
    rc = sched_compile(sched_yaml_root(doc), out, err, err_cap);
    sched_yaml_free(doc); /* transient arena, freed after compile by design */
    return rc;
}

/*
 * envelope_provenance.c — see envelope_provenance.h for the contract. Pure
 * C11, no ESP-IDF dependency, so the host test (tests/
 * envelope_provenance_host.c) compiles this exact file.
 */

#include "envelope_provenance.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "payload_scalar.h"

/* A string is safe to splice into the envelope JSON without escaping only
 * when it is non-empty and drawn from [A-Za-z0-9_.:-]. The schedule compiler
 * enforces this on macro ids/names/filenames; the re-check here is what
 * covers workbook_version_id (compiled before that rule existed) and any
 * future producer of the port struct. */
static bool prov_str_safe(const char *s)
{
    if (s[0] == '\0') return false;
    for (; *s != '\0'; s++) {
        char ch = *s;
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
                  ch == ':' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

/* Worst case (callers size their buffer from this; per-row filtering can only
 * SHRINK the output, so the pre-filtering worst case still bounds it):
 *   workbook key:  24 + 63        =   87 B   "workbook_version_id":"<63>",
 *                  (21 name chars + 2 quotes + colon + 2 quotes + comma)
 *   macros key:    10             =   10 B   "macros":[
 *   8 entries    × 167            = 1336 B   {"id":"<39>","name":"<47>",
 *                  "filename":"<47>"}, — 34 B of punctuation and key text
 *                  plus 39/47/47 value chars (39 is the id[40] buffer width;
 *                  the compiler's uuid cap is 36, so reality is smaller). The
 *                  last entry's trailing comma is rewritten to "]" in place.
 *   closing comma  + 1            =    1 B
 *   total 1434 B + NUL → 1435 B. PROVENANCE_PART_CAP is 1440, so the real
 *   headroom is 5 B — thin enough that widening any field width must be
 *   accompanied by raising this cap. The render loop clamps on `off` rather
 *   than trusting this arithmetic, so getting it wrong truncates the part
 *   visibly instead of writing past the caller's buffer. */
#define PROVENANCE_PART_CAP 1440

/* sched_prov_cond_field_t → the key path payload_scalar understands. The
 * compiler (sched_compile.c) admits exactly these fields, so a port struct
 * carrying anything else is corrupt and the macro must not apply. */
static const char *cond_key_path(uint8_t field)
{
    switch (field) {
    case SCHED_PROV_COND_FIELD_SCHEMA:        return "schema";
    case SCHED_PROV_COND_FIELD_TAG:           return "tag";
    case SCHED_PROV_COND_FIELD_CHANNEL:       return "channel";
    case SCHED_PROV_COND_FIELD_DEVICE:        return "device";
    case SCHED_PROV_COND_FIELD_SENSOR_ID:     return "sensor_id";
    case SCHED_PROV_COND_FIELD_PROTOCOL_NAME: return "protocol.name";
    case SCHED_PROV_COND_FIELD_PROTOCOL_TAG:  return "protocol.tag";
    default:                                  return NULL;
    }
}

/* Every addressable field, resolved AT MOST ONCE per publish.
 *
 * payload_scalar restarts at byte 0 on every call, which is free for the
 * canonical v3 families (`schema` is the first member of all four builders,
 * and channel/device/sensor_id/tag/protocol all sit in the first few hundred
 * bytes) but expensive on a v2 backlog row, where `schema` is ABSENT and the
 * scan has to walk to the object's closing brace. That case is not exotic: it
 * is exactly the documented legacy idiom (four `schema neq` conditions) plus
 * one `schema eq` per family macro, so a 64 KiB v2 record would otherwise pay
 * up to 32 full-payload scans (~262 µs/row measured on host for eight; single-
 * digit ms on the S3 with the payload in PSRAM) on the sync_runner drain path.
 * Caching drops the worst case to 7 scans, and to 1 for the realistic
 * three-family header. ~350 B of stack, still no heap. */
#define COND_FIELD_COUNT (SCHED_PROV_COND_FIELD_PROTOCOL_TAG + 1)

typedef struct {
    const char *payload_json;
    bool resolved[COND_FIELD_COUNT]; /* scan attempted */
    bool present[COND_FIELD_COUNT];  /* scan found a fitting string */
    char value[COND_FIELD_COUNT][48];
} row_fields_t;

static void row_fields_init(row_fields_t *rf, const char *payload_json)
{
    rf->payload_json = payload_json;
    for (int f = 0; f < COND_FIELD_COUNT; f++) {
        rf->resolved[f] = false;
        rf->present[f] = false;
    }
}

/* The row's value for `field`, or NULL when absent/unreadable. */
static const char *row_field(row_fields_t *rf, uint8_t field)
{
    if (field >= COND_FIELD_COUNT) return NULL; /* corrupt port struct */
    if (!rf->resolved[field]) {
        const char *path = cond_key_path(field);
        rf->resolved[field] = true;
        rf->present[field] = path != NULL && rf->payload_json != NULL &&
            payload_scalar(rf->payload_json, path, rf->value[field],
                           sizeof(rf->value[field]));
    }
    return rf->present[field] ? rf->value[field] : NULL;
}

/* All of the macro's conditions against the row being published. ABSENT
 * field semantics: eq is false, neq is true — that asymmetry is what lets a
 * "legacy/v2" macro be expressed as `schema neq` each of the four canonical
 * v3 prefixes (v2 payloads carry no schema key at all; exactly the
 * 4-condition cap). A value too long for the scan buffer can never equal a
 * compiler-capped (≤47 chars) condition value, so payload_scalar's false on
 * overflow folds into "absent" with the same eq/neq outcome. */
static bool macro_applies(const schedule_provenance_macro_t *m,
                          row_fields_t *rf)
{
    for (int j = 0; j < m->cond_count && j < SCHEDULE_PROVENANCE_MAX_MACRO_CONDS; j++) {
        const char *val = row_field(rf, m->conds[j].field);
        bool eq = val != NULL && strcmp(val, m->conds[j].value) == 0;
        if (m->conds[j].op == SCHED_PROV_COND_OP_EQ && !eq) return false;
        if (m->conds[j].op == SCHED_PROV_COND_OP_NEQ && eq) return false;
    }
    return true;
}

int envelope_provenance_part(const schedule_provenance_t *prov,
                             const char *payload_json, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (prov == NULL || cap < PROVENANCE_PART_CAP) return 0;

    size_t off = 0;
    if (prov_str_safe(prov->workbook_version_id)) {
        off += (size_t)snprintf(out + off, cap - off,
                                "\"workbook_version_id\":\"%s\",",
                                prov->workbook_version_id);
    }
    if (prov->macro_count == 0) return (int)off;
    if (prov->macro_count > SCHEDULE_PROVENANCE_MAX_MACROS) {
        out[0] = '\0';
        return 0;
    }
    /* Per-row routing: render only the macros whose conditions all hold for
     * THIS payload. The unsafe-string pre-check covers only those — an
     * excluded macro must not drop keys from a row it does not apply to. */
    bool applies[SCHEDULE_PROVENANCE_MAX_MACROS];
    int rendered = 0;
    row_fields_t rf;
    row_fields_init(&rf, payload_json);
    for (int i = 0; i < prov->macro_count; i++) {
        applies[i] = macro_applies(&prov->macros[i], &rf);
        if (!applies[i]) continue;
        rendered++;
        if (!prov_str_safe(prov->macros[i].id) ||
            !prov_str_safe(prov->macros[i].name) ||
            !prov_str_safe(prov->macros[i].filename)) {
            return (int)off; /* workbook key may still stand on its own */
        }
    }
    if (rendered == 0) return (int)off; /* omit the macros key entirely */
    /* snprintf returns what it WOULD have written, so an `off` that passes
     * `cap` makes the NEXT call's `cap - off` underflow to a huge size_t and
     * write past `out`. The arithmetic above says that cannot happen (5 B of
     * headroom at the worst case), but 5 B is exactly the kind of margin a
     * later field-width change eats without anyone recomputing it, so every
     * write is bounds-checked and a would-be overflow emits NOTHING. An empty
     * part reads to the caller as "this schedule declares no provenance",
     * which is the byte-locked, already-tested path — so a broken sizing shows
     * up as missing provenance rather than as a smashed envelope. The +2 of
     * slack reserves the closing "]," that still has to be written. */
#define PART_APPEND(...)                                                     \
    do {                                                                     \
        int _w = snprintf(out + off, cap - off, __VA_ARGS__);                \
        if (_w < 0 || (size_t)_w + 2 > cap - off) { out[0] = '\0'; return 0; } \
        off += (size_t)_w;                                                   \
    } while (0)

    PART_APPEND("\"macros\":[");
    for (int i = 0; i < prov->macro_count; i++) {
        if (!applies[i]) continue;
        PART_APPEND("{\"id\":\"%s\",\"name\":\"%s\",\"filename\":\"%s\"},",
                    prov->macros[i].id, prov->macros[i].name,
                    prov->macros[i].filename);
    }
    out[off - 1] = ']'; /* the last entry's trailing comma */
    PART_APPEND(",");
#undef PART_APPEND
    return (int)off;
}

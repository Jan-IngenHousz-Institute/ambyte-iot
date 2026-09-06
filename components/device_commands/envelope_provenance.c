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
 *   workbook key:  25 + 63        =   88 B   ("workbook_version_id":"…",)
 *   macros key:    10 + 8×166 + 2 = 1338 B   ("macros":[ + per entry
 *                  {"id":"…","name":"…","filename":"…"}, = 33 B of quotes and
 *                  punctuation + 39/47/47-char values (id[40] buffer width;
 *                  the compiler's uuid cap is 36, so reality is smaller), the
 *                  last entry's comma rewritten to "]" + the closing ",")
 *   total 1426 B + NUL → 1427 B, rounded up to 1440 B. */
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

/* All of the macro's conditions against the row being published. ABSENT
 * field semantics: eq is false, neq is true — that asymmetry is what lets a
 * "legacy/v2" macro be expressed as `schema neq` each of the four canonical
 * v3 prefixes (v2 payloads carry no schema key at all; exactly the
 * 4-condition cap). A value too long for the scan buffer can never equal a
 * compiler-capped (≤47 chars) condition value, so payload_scalar's false on
 * overflow folds into "absent" with the same eq/neq outcome. */
static bool macro_applies(const schedule_provenance_macro_t *m,
                          const char *payload_json)
{
    for (int j = 0; j < m->cond_count && j < SCHEDULE_PROVENANCE_MAX_MACRO_CONDS; j++) {
        const char *path = cond_key_path(m->conds[j].field);
        char val[48]; /* condition values are compile-capped at 47 + NUL */
        bool eq = false;
        if (path != NULL && payload_json != NULL &&
            payload_scalar(payload_json, path, val, sizeof(val))) {
            eq = strcmp(val, m->conds[j].value) == 0;
        }
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
    for (int i = 0; i < prov->macro_count; i++) {
        applies[i] = macro_applies(&prov->macros[i], payload_json);
        if (!applies[i]) continue;
        rendered++;
        if (!prov_str_safe(prov->macros[i].id) ||
            !prov_str_safe(prov->macros[i].name) ||
            !prov_str_safe(prov->macros[i].filename)) {
            return (int)off; /* workbook key may still stand on its own */
        }
    }
    if (rendered == 0) return (int)off; /* omit the macros key entirely */
    off += (size_t)snprintf(out + off, cap - off, "\"macros\":[");
    for (int i = 0; i < prov->macro_count; i++) {
        if (!applies[i]) continue;
        off += (size_t)snprintf(out + off, cap - off,
                                "{\"id\":\"%s\",\"name\":\"%s\",\"filename\":\"%s\"},",
                                prov->macros[i].id, prov->macros[i].name,
                                prov->macros[i].filename);
    }
    out[off - 1] = ']'; /* the last entry's trailing comma */
    off += (size_t)snprintf(out + off, cap - off, ",");
    return (int)off;
}

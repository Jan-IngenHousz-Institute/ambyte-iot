/*
 * envelope_provenance.c — see envelope_provenance.h for the contract. Pure
 * C11, no ESP-IDF dependency, so the host test (tests/
 * envelope_provenance_host.c) compiles this exact file.
 */

#include "envelope_provenance.h"

#include <stdbool.h>
#include <stdio.h>

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

/* Worst case (callers size their buffer from this):
 *   workbook key:  25 + 63        =   88 B   ("workbook_version_id":"…",)
 *   macros key:    10 + 8×167 + 2 = 1348 B   ("macros":[ + per entry
 *                  {"id":"…","name":"…","filename":"…"}, with 39/47/47-char
 *                  values + closing "],")
 *   total 1436 B + NUL → 1440 B. */
#define PROVENANCE_PART_CAP 1440

int envelope_provenance_part(const schedule_provenance_t *prov, char *out, size_t cap)
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
    for (int i = 0; i < prov->macro_count; i++) {
        if (!prov_str_safe(prov->macros[i].id) ||
            !prov_str_safe(prov->macros[i].name) ||
            !prov_str_safe(prov->macros[i].filename)) {
            return (int)off; /* workbook key may still stand on its own */
        }
    }
    off += (size_t)snprintf(out + off, cap - off, "\"macros\":[");
    for (int i = 0; i < prov->macro_count; i++) {
        off += (size_t)snprintf(out + off, cap - off,
                                "{\"id\":\"%s\",\"name\":\"%s\",\"filename\":\"%s\"},",
                                prov->macros[i].id, prov->macros[i].name,
                                prov->macros[i].filename);
    }
    out[off - 1] = ']'; /* the last entry's trailing comma */
    off += (size_t)snprintf(out + off, cap - off, ",");
    return (int)off;
}

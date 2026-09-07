/*
 * payload_scalar.c — see payload_scalar.h for the contract. Pure C11, no
 * heap, no cJSON, so the host test (tests/payload_scalar_host.c) compiles
 * this exact file next to the real payload_v3.c builders.
 */

#include "payload_scalar.h"

#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* One past the closing quote, or NULL on a truncated string. Backslash skips
 * the escaped byte, so a \" inside a value cannot terminate the scan early. */
static const char *skip_string(const char *p)
{
    p++; /* opening quote */
    while (*p != '\0') {
        if (*p == '\\') {
            if (p[1] == '\0') return NULL;
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

/* Find key among the IMMEDIATE members of the object whose content starts at
 * p (p is just past its '{'). Returns the value start (one past ':') or NULL
 * when the key is absent or the object proves malformed/truncated. Nested
 * containers are walked with a depth counter and their members never match —
 * that is what keeps telemetry's attached_sensors[].channel from answering a
 * top-level "channel" query, and the top-level "tag" from answering a
 * "protocol.tag" one. Strings are skipped wholesale, so a value that happens
 * to contain the text "key" cannot be mistaken for a key. */
static const char *find_key(const char *p, const char *key)
{
    const size_t klen = strlen(key);
    int depth = 1; /* inside the searched object */
    while (*p != '\0') {
        char ch = *p;
        if (ch == '"') {
            const char *end = skip_string(p);
            if (end == NULL) return NULL;
            /* a key is the only string legitimately followed by ':' */
            if (depth == 1 && (size_t)(end - p) == klen + 2 &&
                strncmp(p + 1, key, klen) == 0 && *skip_ws(end) == ':') {
                return skip_ws(end) + 1;
            }
            p = end;
            continue;
        }
        if (ch == '{' || ch == '[') depth++;
        else if (ch == '}' || ch == ']') {
            depth--;
            if (depth == 0) return NULL; /* object ended: key absent */
        }
        p++;
    }
    return NULL; /* truncated */
}

static bool copy_string_value(const char *p, char *out, size_t cap)
{
    p = skip_ws(p);
    if (*p != '"') return false; /* non-string value */
    p++;
    size_t n = 0;
    while (*p != '\0' && *p != '"') {
        char ch = *p;
        if (ch == '\\') { /* minimal unescape: collapse \" and \\ (see .h) */
            if (p[1] == '\0') return false;
            ch = p[1];
            p += 2;
        } else {
            p++;
        }
        if (n + 1 >= cap) return false;
        out[n++] = ch;
    }
    if (*p != '"') return false; /* truncated string */
    out[n] = '\0';
    return true;
}

/* Widest path segment these buffers must hold, +1 for the NUL. The caller's
 * key paths come from ONE closed set: cond_key_path() in
 * components/device_commands/envelope_provenance.c, which mirrors
 * sched_compile.c's `when:` field table. The longest segment today is
 * "sensor_id" (9) / "protocol" (8). This coupling is worth pinning because
 * the failure mode is silent: a ≥ 12-char field name makes payload_scalar
 * return false, which reads as "absent" and flips every eq/neq using it,
 * per row, with nothing failing at compile or install time. Adding a longer
 * field means raising this and the two buffers below together. */
#define PAYLOAD_SCALAR_SEGMENT_CAP 12
_Static_assert(PAYLOAD_SCALAR_SEGMENT_CAP > sizeof("sensor_id") - 1 &&
               PAYLOAD_SCALAR_SEGMENT_CAP > sizeof("protocol") - 1,
               "path segment buffers must hold every addressable field name");

bool payload_scalar(const char *json, const char *key_path, char *out, size_t cap)
{
    if (json == NULL || key_path == NULL || out == NULL || cap == 0) return false;
    out[0] = '\0';

    /* key_path: "key" or "parent.key" (one level, exactly one dot) */
    char parent[PAYLOAD_SCALAR_SEGMENT_CAP], key[PAYLOAD_SCALAR_SEGMENT_CAP];
    const char *dot = strchr(key_path, '.');
    if (dot == NULL) {
        if (strlen(key_path) >= sizeof(key)) return false;
        parent[0] = '\0';
        strcpy(key, key_path);
    } else {
        if (strchr(dot + 1, '.') != NULL) return false; /* deeper paths unsupported */
        size_t plen = (size_t)(dot - key_path);
        size_t klen = strlen(dot + 1);
        if (plen == 0 || plen >= sizeof(parent) ||
            klen == 0 || klen >= sizeof(key)) return false;
        memcpy(parent, key_path, plen);
        parent[plen] = '\0';
        memcpy(key, dot + 1, klen + 1);
    }

    const char *p = skip_ws(json);
    if (*p != '{') return false; /* payloads are always a single object */
    const char *value;
    if (parent[0] != '\0') {
        /* locate the parent object, then search only within its bounds:
         * find_key's depth counter returns NULL at its closing brace */
        const char *pv = find_key(p + 1, parent);
        if (pv == NULL || *skip_ws(pv) != '{') return false;
        value = find_key(skip_ws(pv) + 1, key);
    } else {
        value = find_key(p + 1, key);
    }
    if (value == NULL) return false;
    return copy_string_value(value, out, cap);
}

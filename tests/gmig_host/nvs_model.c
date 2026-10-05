/* G-MIG host NVS model: see stubs/nvs.h for the semantics contract.
 *
 * State file ./nvs.db (CWD = the device state directory), rewritten
 * atomically (tmp + fsync + rename) after every successful mutation:
 *   @ <namespace>                               namespace exists
 *   <namespace> <key> <type> <hex | ->          one entry
 * gmig.py converts it to/from the human/JSON form (nvs.json) and to/from a
 * real device's NVS key dump. Every call is appended to .shim/nvs_ops.jsonl
 * (variant, op, ns, key, type, value hex, rc): the evidence for "which keys
 * each firmware reads and writes". Plain libc; no forced shim include. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvs.h"

#ifndef GMIG_VARIANT
#define GMIG_VARIANT "unknown"
#endif

enum { T_U8 = 0x01, T_I8 = 0x11, T_U16 = 0x02, T_I16 = 0x12, T_U32 = 0x04, T_I32 = 0x14,
       T_U64 = 0x08, T_I64 = 0x18, T_STR = 0x21, T_BLOB = 0x42 };

static const struct { int t; const char *n; } k_types[] = {
    { T_U8, "u8" }, { T_I8, "i8" }, { T_U16, "u16" }, { T_I16, "i16" }, { T_U32, "u32" }, { T_I32, "i32" },
    { T_U64, "u64" }, { T_I64, "i64" }, { T_STR, "str" }, { T_BLOB, "blob" },
};

static const char *tname(int t)
{
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) if (k_types[i].t == t) return k_types[i].n;
    return "?";
}
static int tparse(const char *n)
{
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) if (strcmp(k_types[i].n, n) == 0) return k_types[i].t;
    return -1;
}

#define NS_MAX   64
#define ENT_MAX  512
#define VAL_MAX  4000       /* IDF blob max is larger (multi-page); nothing in scope comes close */
#define H_MAX    32

typedef struct { bool used; char ns[16]; char key[16]; int type; size_t len; uint8_t *val; } ent_t;
typedef struct { bool open; char ns[16]; nvs_open_mode_t mode; } hnd_t;

static char   s_ns[NS_MAX][16];
static int    s_nns = 0;
static ent_t  s_ent[ENT_MAX];
static hnd_t  s_h[H_MAX];
static bool   s_loaded = false;
static unsigned s_type_conflicts = 0;
static FILE  *s_log = NULL;

unsigned gmig_nvs_type_conflicts(void) { return s_type_conflicts; }

static void hexs(char *out, size_t cap, const uint8_t *v, size_t len)
{
    size_t j = 0;
    if (len == 0) { snprintf(out, cap, "-"); return; }
    for (size_t i = 0; i < len && j + 3 < cap; i++) j += (size_t)snprintf(out + j, cap - j, "%02x", v[i]);
    out[j] = '\0';
}

static void oplog(const char *op, const char *ns, const char *key, int type, const void *v, size_t len, esp_err_t rc)
{
    if (s_log == NULL) {
        mkdir(".shim", 0777);
        s_log = fopen(".shim/nvs_ops.jsonl", "a");
        if (s_log == NULL) return;
    }
    static char hx[2 * VAL_MAX + 4];
    hexs(hx, sizeof hx, (const uint8_t *)v, v ? len : 0);
    fprintf(s_log, "{\"fw\":\"%s\",\"op\":\"%s\",\"ns\":\"%s\",\"key\":\"%s\",\"type\":\"%s\",\"hex\":\"%s\",\"rc\":%d}\n",
            GMIG_VARIANT, op, ns ? ns : "", key ? key : "", type > 0 ? tname(type) : "", hx, rc);
    fflush(s_log);
}

static bool ns_exists(const char *ns)
{
    for (int i = 0; i < s_nns; i++) if (strcmp(s_ns[i], ns) == 0) return true;
    return false;
}

static void ns_add(const char *ns)
{
    if (ns_exists(ns) || s_nns >= NS_MAX) return;
    snprintf(s_ns[s_nns++], 16, "%s", ns);
}

static int unhex(const char *h, uint8_t *out, size_t cap)
{
    if (strcmp(h, "-") == 0) return 0;
    size_t n = strlen(h);
    if (n % 2 != 0 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned b;
        if (sscanf(h + 2 * i, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return (int)(n / 2);
}

static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    FILE *f = fopen("nvs.db", "r");
    if (f == NULL) return;
    char line[2 * VAL_MAX + 128];
    static uint8_t buf[VAL_MAX];
    while (fgets(line, sizeof line, f) != NULL) {
        char a[64], b[64], c[16];
        static char d[2 * VAL_MAX + 4];
        if (line[0] == '@') {
            if (sscanf(line, "@ %15s", a) == 1) ns_add(a);
            continue;
        }
        if (sscanf(line, "%15s %15s %15s %8003s", a, b, c, d) != 4) {
            fprintf(stderr, "FATAL: nvs.db: bad line: %s", line);
            abort();
        }
        int t = tparse(c), n = unhex(d, buf, sizeof buf);
        if (t < 0 || n < 0) { fprintf(stderr, "FATAL: nvs.db: bad entry: %s", line); abort(); }
        ns_add(a);
        for (int i = 0; i < ENT_MAX; i++) {
            if (s_ent[i].used) continue;
            s_ent[i].used = true;
            snprintf(s_ent[i].ns, 16, "%s", a);
            snprintf(s_ent[i].key, 16, "%s", b);
            s_ent[i].type = t;
            s_ent[i].len = (size_t)n;
            s_ent[i].val = malloc((size_t)n + 1);
            memcpy(s_ent[i].val, buf, (size_t)n);
            break;
        }
    }
    fclose(f);
}

static esp_err_t save(void)
{
    FILE *f = fopen("nvs.db.tmp", "w");
    if (f == NULL) return ESP_FAIL;
    for (int i = 0; i < s_nns; i++) fprintf(f, "@ %s\n", s_ns[i]);
    static char hx[2 * VAL_MAX + 4];
    for (int i = 0; i < ENT_MAX; i++) {
        if (!s_ent[i].used) continue;
        hexs(hx, sizeof hx, s_ent[i].val, s_ent[i].len);
        fprintf(f, "%s %s %s %s\n", s_ent[i].ns, s_ent[i].key, tname(s_ent[i].type), hx);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename("nvs.db.tmp", "nvs.db") == 0 ? ESP_OK : ESP_FAIL;
}

static bool name_ok(const char *s) { return s != NULL && s[0] != '\0' && strlen(s) <= 15; }

static hnd_t *hget(nvs_handle_t h)
{
    if (h == 0 || h > H_MAX || !s_h[h - 1].open) return NULL;
    return &s_h[h - 1];
}

static ent_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < ENT_MAX; i++) {
        if (s_ent[i].used && strcmp(s_ent[i].ns, ns) == 0 && strcmp(s_ent[i].key, key) == 0) return &s_ent[i];
    }
    return NULL;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    load();
    esp_err_t rc;
    if (out == NULL) rc = ESP_ERR_INVALID_ARG;
    else if (!name_ok(ns)) rc = ns && strlen(ns) > 15 ? ESP_ERR_NVS_KEY_TOO_LONG : ESP_ERR_NVS_INVALID_NAME;
    else if (mode == NVS_READONLY && !ns_exists(ns)) rc = ESP_ERR_NVS_NOT_FOUND;
    else {
        rc = ESP_ERR_NVS_INVALID_HANDLE;
        if (mode == NVS_READWRITE && !ns_exists(ns)) { ns_add(ns); if (save() != ESP_OK) { oplog("open", ns, NULL, 0, NULL, 0, ESP_FAIL); return ESP_FAIL; } }
        for (int i = 0; i < H_MAX; i++) {
            if (s_h[i].open) continue;
            s_h[i].open = true;
            s_h[i].mode = mode;
            snprintf(s_h[i].ns, 16, "%s", ns);
            *out = (nvs_handle_t)(i + 1);
            rc = ESP_OK;
            break;
        }
    }
    oplog(mode == NVS_READONLY ? "open_ro" : "open_rw", ns, NULL, 0, NULL, 0, rc);
    return rc;
}

void nvs_close(nvs_handle_t h)
{
    hnd_t *x = hget(h);
    if (x) x->open = false;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    hnd_t *x = hget(h);
    esp_err_t rc = x == NULL ? ESP_ERR_NVS_INVALID_HANDLE : ESP_OK;   /* sets are already durable */
    oplog("commit", x ? x->ns : NULL, NULL, 0, NULL, 0, rc);
    return rc;
}

static esp_err_t do_set(nvs_handle_t h, const char *key, int type, const void *v, size_t len)
{
    hnd_t *x = hget(h);
    esp_err_t rc = ESP_OK;
    if (x == NULL) rc = ESP_ERR_NVS_INVALID_HANDLE;
    else if (x->mode != NVS_READWRITE) rc = ESP_ERR_NVS_READ_ONLY;
    else if (!name_ok(key)) rc = key && strlen(key) > 15 ? ESP_ERR_NVS_KEY_TOO_LONG : ESP_ERR_NVS_INVALID_NAME;
    else if (len > VAL_MAX) rc = ESP_ERR_NVS_VALUE_TOO_LONG;
    else {
        ent_t *e = find(x->ns, key);
        if (e != NULL && e->type != type) {
            s_type_conflicts++;
            rc = ESP_ERR_NVS_TYPE_MISMATCH;
            oplog("type_conflict", x->ns, key, type, v, len, rc);
            return rc;
        }
        if (e == NULL) {
            for (int i = 0; i < ENT_MAX; i++) if (!s_ent[i].used) { e = &s_ent[i]; break; }
            if (e == NULL) rc = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
            else {
                e->used = true;
                snprintf(e->ns, 16, "%s", x->ns);
                snprintf(e->key, 16, "%s", key);
                e->type = type;
                e->val = NULL;
            }
        }
        if (e != NULL) {
            free(e->val);
            e->val = malloc(len + 1);
            memcpy(e->val, v, len);
            e->len = len;
            rc = save();
        }
    }
    oplog("set", x ? x->ns : NULL, key, type, v, len, rc);
    return rc;
}

static esp_err_t do_get(nvs_handle_t h, const char *key, int type, void *out, size_t want)
{
    hnd_t *x = hget(h);
    esp_err_t rc;
    ent_t *e = NULL;
    if (x == NULL) rc = ESP_ERR_NVS_INVALID_HANDLE;
    else if (!name_ok(key)) rc = ESP_ERR_NVS_INVALID_NAME;
    else if ((e = find(x->ns, key)) == NULL) rc = ESP_ERR_NVS_NOT_FOUND;
    else if (e->type != type) rc = ESP_ERR_NVS_TYPE_MISMATCH;
    else if (e->len != want) rc = ESP_ERR_NVS_INVALID_LENGTH;   /* impossible for a typed entry */
    else { memcpy(out, e->val, want); rc = ESP_OK; }
    oplog("get", x ? x->ns : NULL, key, type, rc == ESP_OK ? out : NULL, want, rc);
    return rc;
}

static esp_err_t do_get_var(nvs_handle_t h, const char *key, int type, void *out, size_t *len)
{
    hnd_t *x = hget(h);
    esp_err_t rc;
    ent_t *e = NULL;
    if (x == NULL) rc = ESP_ERR_NVS_INVALID_HANDLE;
    else if (len == NULL) rc = ESP_ERR_INVALID_ARG;
    else if (!name_ok(key)) rc = ESP_ERR_NVS_INVALID_NAME;
    else if ((e = find(x->ns, key)) == NULL) rc = ESP_ERR_NVS_NOT_FOUND;
    else if (e->type != type) rc = ESP_ERR_NVS_TYPE_MISMATCH;
    else if (out == NULL) { *len = e->len; rc = ESP_OK; }
    else if (*len < e->len) { *len = e->len; rc = ESP_ERR_NVS_INVALID_LENGTH; }
    else { memcpy(out, e->val, e->len); *len = e->len; rc = ESP_OK; }
    oplog("get", x ? x->ns : NULL, key, type, (rc == ESP_OK && out) ? out : NULL, (e && rc == ESP_OK) ? e->len : 0, rc);
    return rc;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    hnd_t *x = hget(h);
    esp_err_t rc;
    ent_t *e = NULL;
    if (x == NULL) rc = ESP_ERR_NVS_INVALID_HANDLE;
    else if (x->mode != NVS_READWRITE) rc = ESP_ERR_NVS_READ_ONLY;
    else if ((e = find(x->ns, key)) == NULL) rc = ESP_ERR_NVS_NOT_FOUND;
    else { e->used = false; free(e->val); e->val = NULL; rc = save(); }
    oplog("erase_key", x ? x->ns : NULL, key, 0, NULL, 0, rc);
    return rc;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    hnd_t *x = hget(h);
    esp_err_t rc;
    if (x == NULL) rc = ESP_ERR_NVS_INVALID_HANDLE;
    else if (x->mode != NVS_READWRITE) rc = ESP_ERR_NVS_READ_ONLY;
    else {
        for (int i = 0; i < ENT_MAX; i++) {
            if (s_ent[i].used && strcmp(s_ent[i].ns, x->ns) == 0) { s_ent[i].used = false; free(s_ent[i].val); s_ent[i].val = NULL; }
        }
        rc = save();
    }
    oplog("erase_all", x ? x->ns : NULL, NULL, 0, NULL, 0, rc);
    return rc;
}

#define SCALAR(sfx, T, TY) \
    esp_err_t nvs_get_##sfx(nvs_handle_t h, const char *k, T *o) { return do_get(h, k, TY, o, sizeof(T)); } \
    esp_err_t nvs_set_##sfx(nvs_handle_t h, const char *k, T v) { return do_set(h, k, TY, &v, sizeof(T)); }
SCALAR(u8, uint8_t, T_U8)
SCALAR(u16, uint16_t, T_U16)
SCALAR(u32, uint32_t, T_U32)
SCALAR(i32, int32_t, T_I32)
SCALAR(u64, uint64_t, T_U64)
SCALAR(i64, int64_t, T_I64)

esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *len) { return do_get_var(h, k, T_STR, o, len); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{
    if (v == NULL) return ESP_ERR_INVALID_ARG;
    return do_set(h, k, T_STR, v, strlen(v) + 1);
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *len) { return do_get_var(h, k, T_BLOB, o, len); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    if (v == NULL && len > 0) return ESP_ERR_INVALID_ARG;
    return do_set(h, k, T_BLOB, v, len);
}

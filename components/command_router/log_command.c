/*
 * log_command.c — log_list / log_read (contract: include/log_command.h).
 *
 * Two task contexts, deliberately split:
 *   esp-mqtt task  : log_command_handle() — parse, validate, copy a bounded
 *                    request, enqueue. No I/O, no SD, no event_log.
 *   maint worker   : execute() — maintenance lock → SD-state refusals →
 *                    sdcard_io_begin → gen sample → opendir/fopen "rb"/fread →
 *                    gen sample → io_end → maintenance_end → THEN publish. The
 *                    reply is published after every SD reference is released,
 *                    so a slow MQTT write can never hold up a low-battery
 *                    unmount.
 *
 * Replies come in two kinds (review finding 4):
 *   - refusals: written into a stack buffer by hand, no allocation at all, so
 *     even an exhausted heap gets an explicit answer;
 *   - successes: cJSON, with EVERY add checked. Any failure discards the
 *     whole object and sends the "no_mem" refusal instead. A partial listing
 *     or a read without its data can never be reported as ok:true.
 * All heap use goes through cJSON_malloc/cJSON_free (one allocator, one
 * injection point for the allocation-failure tests).
 */
#include "log_command.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>   /* qsort only: all heap use is cJSON_malloc */
#include <string.h>
#include <sys/stat.h>

static log_command_config_t s_config;
/* One log operation at a time: a QoS1 duplicate or a second operator gets
 * "busy" at once instead of queueing a second 8 KiB read behind the first. */
static atomic_bool s_busy;

typedef struct {
    char    id[65];
    char    name[LOG_NAME_MAX + 1];
    char    expect_gen[LOG_GEN_MAX];     /* "" = no expectation */
    int64_t offset;
    int64_t length;
    bool    read;
} request_t;

/* ── pure helpers ──────────────────────────────────────────────────────── */

bool log_command_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.') return false;
    size_t n = strnlen(name, LOG_NAME_MAX + 1);
    if (n > LOG_NAME_MAX) return false;
    if (strstr(name, "..") != NULL) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        /* Allow-list, not a deny-list: '/', '\\', ':' (drive syntax), control
         * bytes, quotes and anything non-ASCII are all simply not in it. */
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

const char *log_command_resolve(int64_t size, int64_t offset, int64_t length,
                                int64_t *start, int64_t *len)
{
    if (size < 0) return "stat_failed";
    if (length <= 0) return "invalid_length";
    if (length > LOG_READ_MAX_LEN) length = LOG_READ_MAX_LEN;
    int64_t s = offset >= 0 ? offset : size + offset;   /* negative = tail */
    if (s < 0) s = 0;                                    /* tail longer than the file */
    if (s > size) return "offset_past_eof";
    *start = s;
    *len = (size - s) < length ? (size - s) : length;
    return NULL;
}

size_t log_command_base64(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = 4 * ((n + 2) / 3);
    if (out == NULL || cap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = in[i];
        const uint32_t b = i + 1 < n ? in[i + 1] : 0;
        const uint32_t c = i + 2 < n ? in[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out[o++] = k[(v >> 18) & 63];
        out[o++] = k[(v >> 12) & 63];
        out[o++] = i + 1 < n ? k[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? k[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static void gen_now(log_gen_t *g)
{
    memset(g, 0, sizeof *g);
    if (s_config.gen) s_config.gen(g);
}

static void gen_string(const log_gen_t *g, char *out, size_t cap)
{
    snprintf(out, cap, "%lu.%08lx.%lu.%lu", (unsigned long)g->boot, (unsigned long)g->nonce,
             (unsigned long)g->mount, (unsigned long)g->rotation);
}

static bool gen_equal(const log_gen_t *a, const log_gen_t *b)
{
    return a->boot == b->boot && a->nonce == b->nonce && a->mount == b->mount && a->rotation == b->rotation;
}

/* ── allocation-free refusals ──────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   ok;
} sbuf_t;

static void sb_raw(sbuf_t *b, const char *s)
{
    size_t n = strlen(s);
    if (!b->ok || b->len + n >= b->cap) { b->ok = false; return; }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* JSON string with the same escaping rules cJSON uses (quote, backslash,
 * control bytes; other bytes verbatim). */
static void sb_str(sbuf_t *b, const char *s)
{
    sb_raw(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p && b->ok; p++) {
        char e[8];
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = (char)*p; e[2] = '\0'; }
        else if (*p < 0x20) snprintf(e, sizeof e, "\\u%04x", *p);
        else { e[0] = (char)*p; e[1] = '\0'; }
        sb_raw(b, e);
    }
    sb_raw(b, "\"");
}

/* {"type","id","ok":false,"detail",("name"|"dir"),["gen"],["size"]} in a stack
 * buffer: id ≤ 64 B (≤ 384 escaped), name allow-listed, dir a fixed path. */
static void send_refusal(const request_t *r, const char *detail, const log_gen_t *gen, int64_t size)
{
    char buf[768];
    sbuf_t b = {.buf = buf, .cap = sizeof buf, .len = 0, .ok = true};
    buf[0] = '\0';
    sb_raw(&b, r->read ? "{\"type\":\"log_read_result\",\"id\":" : "{\"type\":\"log_list_result\",\"id\":");
    sb_str(&b, r->id);
    sb_raw(&b, ",\"ok\":false,\"detail\":");
    sb_str(&b, detail);
    if (r->read) {
        sb_raw(&b, ",\"name\":");
        sb_str(&b, r->name);
    } else {
        sb_raw(&b, ",\"dir\":");
        sb_str(&b, s_config.dir ? s_config.dir : "");
    }
    if (gen != NULL) {
        char g[LOG_GEN_MAX];
        gen_string(gen, g, sizeof g);
        sb_raw(&b, ",\"gen\":");
        sb_str(&b, g);
    }
    if (size >= 0) {
        char n[32];
        snprintf(n, sizeof n, ",\"size\":%lld", (long long)size);
        sb_raw(&b, n);
    }
    sb_raw(&b, "}");
    if (b.ok && s_config.reply) s_config.reply(buf);
}

/* ── success replies (every cJSON operation checked) ───────────────────── */

/* Common header of a success reply, or NULL on any allocation failure. */
static cJSON *success_base(const request_t *r, const log_gen_t *gen)
{
    char g[LOG_GEN_MAX];
    gen_string(gen, g, sizeof g);
    cJSON *j = cJSON_CreateObject();
    if (j == NULL) return NULL;
    if (!cJSON_AddStringToObject(j, "type", r->read ? "log_read_result" : "log_list_result") ||
        !cJSON_AddStringToObject(j, "id", r->id) ||
        !cJSON_AddBoolToObject(j, "ok", true) ||
        !cJSON_AddStringToObject(j, "detail", "ok") ||
        !cJSON_AddStringToObject(j, r->read ? "name" : "dir", r->read ? r->name : (s_config.dir ? s_config.dir : "")) ||
        !cJSON_AddStringToObject(j, "gen", g)) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

/* Result of one SD operation: a complete success JSON, or a refusal. */
typedef struct {
    char       *json;      /* cJSON_malloc'd success reply, or NULL */
    const char *refusal;   /* detail when json == NULL */
    int64_t     size;      /* for offset_past_eof, else -1 */
    bool        gen_known;
    log_gen_t   gen;
} outcome_t;

static outcome_t refuse(const char *detail)
{
    outcome_t o = {.json = NULL, .refusal = detail, .size = -1};
    return o;
}

typedef struct {
    char     name[LOG_NAME_MAX + 1];
    uint32_t size;
} list_entry_t;

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const list_entry_t *)a)->name, ((const list_entry_t *)b)->name);
}

static bool join_path(char *out, size_t cap, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", s_config.dir, name);
    return n > 0 && (size_t)n < cap;
}

static outcome_t do_list(const request_t *r, const log_gen_t *before)
{
    DIR *d = opendir(s_config.dir);
    if (d == NULL) return refuse("dir_unavailable");
    list_entry_t *e = cJSON_malloc(LOG_LIST_MAX_FILES * sizeof *e);
    if (e == NULL) {
        closedir(d);
        return refuse("no_mem");
    }
    size_t count = 0;
    unsigned omitted = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char path[300];
        struct stat st;
        /* Listed = readable by log_read: a name log_read would refuse, a
         * directory, or a 33rd file is counted in `omitted`, not shown. */
        if (!log_command_name_valid(name) || !join_path(path, sizeof path, name) ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode) || count >= LOG_LIST_MAX_FILES) {
            omitted++;
            continue;
        }
        memcpy(e[count].name, name, strlen(name) + 1);
        e[count].size = (uint32_t)st.st_size;
        count++;
    }
    closedir(d);
    qsort(e, count, sizeof *e, entry_cmp);   /* FAT directory order is not meaningful */

    outcome_t o = refuse("no_mem");
    cJSON *j = success_base(r, before);
    cJSON *files = j ? cJSON_AddArrayToObject(j, "files") : NULL;
    bool ok = files != NULL;
    for (size_t i = 0; ok && i < count; i++) {
        cJSON *f = cJSON_CreateObject();
        ok = f != NULL &&
             cJSON_AddStringToObject(f, "name", e[i].name) != NULL &&
             cJSON_AddNumberToObject(f, "size", e[i].size) != NULL;
        if (ok) ok = cJSON_AddItemToArray(files, f);
        if (!ok) cJSON_Delete(f);            /* not attached: ours to free */
    }
    ok = ok && cJSON_AddNumberToObject(j, "omitted", omitted) != NULL;
    if (ok) {
        o.json = cJSON_PrintUnformatted(j);
        if (o.json) o.refusal = NULL;
    }
    cJSON_Delete(j);
    cJSON_free(e);
    return o;
}

/* The name must be an entry of the directory itself, not merely a string
 * that resolves to some file: validation + membership, belt and braces. */
static bool found_in_dir(const char *name)
{
    DIR *d = opendir(s_config.dir);
    if (d == NULL) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(d)) != NULL) found = strcmp(de->d_name, name) == 0;
    closedir(d);
    return found;
}

static outcome_t do_read(const request_t *r, const log_gen_t *before)
{
    if (!found_in_dir(r->name)) return refuse("not_found");
    char path[300];
    if (!join_path(path, sizeof path, r->name)) return refuse("not_found");
    FILE *f = fopen(path, "rb");                 /* read-only, always */
    if (f == NULL) return refuse("open_failed");
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return refuse("stat_failed");
    }
    if (!S_ISREG(st.st_mode)) {
        fclose(f);
        return refuse("not_a_file");
    }
    const int64_t size = (int64_t)st.st_size;
    int64_t start = 0, len = 0;
    const char *bad = log_command_resolve(size, r->offset, r->length, &start, &len);
    if (bad != NULL) {
        fclose(f);
        outcome_t o = refuse(bad);
        o.size = size;
        return o;
    }
    uint8_t *raw = cJSON_malloc(len > 0 ? (size_t)len : 1U);
    if (raw == NULL) {
        fclose(f);
        return refuse("no_mem");
    }
    size_t got = 0;
    if (len > 0) {
        if (fseek(f, (long)start, SEEK_SET) != 0) {
            fclose(f);
            cJSON_free(raw);
            return refuse("seek_failed");
        }
        got = fread(raw, 1, (size_t)len, f);
        if (got < (size_t)len) {
            /* fstat promised `len` bytes at `start`. Fewer without an error
             * means the file shrank under us (a truncation the rotation
             * seqlock does not see): never return it as a valid chunk, whose
             * eof flag would be wrong (review round 2, finding 5). */
            const bool err = ferror(f) != 0;
            fclose(f);
            cJSON_free(raw);
            return refuse(err ? "read_failed" : "short_read");
        }
    }
    fclose(f);

    outcome_t o = refuse("no_mem");
    cJSON *j = success_base(r, before);
    const bool ok = j != NULL &&
        cJSON_AddNumberToObject(j, "size", (double)size) != NULL &&
        cJSON_AddNumberToObject(j, "offset", (double)start) != NULL &&
        cJSON_AddNumberToObject(j, "length", (double)got) != NULL &&
        cJSON_AddBoolToObject(j, "eof", start + (int64_t)got >= size) != NULL &&
        cJSON_AddStringToObject(j, "encoding", "base64") != NULL;
    char *head = ok ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (head != NULL) {
        /* Splice `,"data":"<base64>"}` onto cJSON's (correctly escaped)
         * header instead of handing cJSON an 11 KiB string it would copy
         * twice more. */
        const size_t hl = strlen(head);
        const size_t b64 = 4 * ((got + 2) / 3);
        char *out = cJSON_malloc(hl + b64 + 16);
        if (out != NULL && hl > 0 && head[hl - 1] == '}') {
            memcpy(out, head, hl - 1);           /* drop the closing '}' */
            size_t n = hl - 1;
            memcpy(out + n, ",\"data\":\"", 9);
            n += 9;
            n += log_command_base64(raw, got, out + n, b64 + 1);
            memcpy(out + n, "\"}", 3);
            o.json = out;
            o.refusal = NULL;
        } else {
            cJSON_free(out);
        }
        cJSON_free(head);
    }
    cJSON_free(raw);
    return o;
}

static void execute(void *arg)
{
    request_t *r = arg;
    outcome_t o = refuse("maintenance_active");
    if (s_config.maintenance_begin && s_config.maintenance_end && s_config.maintenance_begin()) {
        /* Order matters: a parked card is also unmounted, and the operator
         * must see WHY (low battery), not just "absent". */
        o = refuse(NULL);
        if (s_config.sd_parked && s_config.sd_parked())            o.refusal = "sd_parked";
        else if (!s_config.sd_mounted || !s_config.sd_mounted())   o.refusal = "sd_absent";
        else if (s_config.sd_io_lost && s_config.sd_io_lost())     o.refusal = "sd_io_lost";
        else if (!s_config.io_begin || !s_config.io_begin())       o.refusal = "sd_busy";
        if (o.refusal == NULL) {
            log_gen_t before, after;
            gen_now(&before);
            char g[LOG_GEN_MAX];
            gen_string(&before, g, sizeof g);
            if (before.rotation & 1U) {
                o = refuse("rotation_in_progress");
            } else if (r->read && r->expect_gen[0] && strcmp(r->expect_gen, g) != 0) {
                /* The basename no longer names the bytes the earlier chunks
                 * came from. */
                o = refuse("file_rotated");
            } else {
                o = r->read ? do_read(r, &before) : do_list(r, &before);
                gen_now(&after);
                if (!gen_equal(&before, &after)) {
                    /* A rename ran while we read: these bytes may belong to
                     * two files. Discard them. */
                    cJSON_free(o.json);
                    o = refuse("file_rotated");
                    before = after;
                }
            }
            o.gen_known = true;
            o.gen = before;
            s_config.io_end();
        }
        s_config.maintenance_end();
    }
    /* Published after every SD reference is gone. */
    if (o.json != NULL) {
        if (s_config.reply) s_config.reply(o.json);
        cJSON_free(o.json);
    } else {
        send_refusal(r, o.refusal, o.gen_known ? &o.gen : NULL, o.size);
    }
    cJSON_free(r);
    atomic_store(&s_busy, false);
}

/* ── MQTT task: parse, validate, enqueue ───────────────────────────────── */

void log_command_init(const log_command_config_t *config)
{
    s_config = *config;
}

/* Integer-valued JSON number within ±2^31 (files are ≤ 2 MiB; 3.5 is not an
 * offset). */
static bool json_int(const cJSON *v, int64_t *out)
{
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return false;
    const double d = v->valuedouble;
    if (d != floor(d) || d > 2147483647.0 || d < -2147483648.0) return false;
    *out = (int64_t)d;
    return true;
}

bool log_command_handle(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    const bool read = strcmp(type->valuestring, "log_read") == 0;
    if (!read && strcmp(type->valuestring, "log_list") != 0) return false;

    request_t r = {.read = read, .length = LOG_READ_DEFAULT_LEN};
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= sizeof r.id) {
        send_refusal(&r, "invalid_id", NULL, -1);
        return true;
    }
    memcpy(r.id, id->valuestring, strlen(id->valuestring) + 1);
    if (read) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (!cJSON_IsString(name) || !log_command_name_valid(name->valuestring)) {
            send_refusal(&r, "invalid_name", NULL, -1);
            return true;
        }
        memcpy(r.name, name->valuestring, strlen(name->valuestring) + 1);
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
        if (offset != NULL && !cJSON_IsNull(offset) && !json_int(offset, &r.offset)) {
            send_refusal(&r, "invalid_offset", NULL, -1);
            return true;
        }
        const cJSON *length = cJSON_GetObjectItemCaseSensitive(root, "length");
        if (length != NULL && !cJSON_IsNull(length) && (!json_int(length, &r.length) || r.length <= 0)) {
            send_refusal(&r, "invalid_length", NULL, -1);
            return true;
        }
        if (r.length > LOG_READ_MAX_LEN) r.length = LOG_READ_MAX_LEN;   /* clamp, reported */
        const cJSON *eg = cJSON_GetObjectItemCaseSensitive(root, "expect_gen");
        if (eg != NULL && !cJSON_IsNull(eg)) {
            if (!cJSON_IsString(eg) || !eg->valuestring[0] || strlen(eg->valuestring) >= sizeof r.expect_gen) {
                send_refusal(&r, "invalid_expect_gen", NULL, -1);
                return true;
            }
            memcpy(r.expect_gen, eg->valuestring, strlen(eg->valuestring) + 1);
        }
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        send_refusal(&r, "busy", NULL, -1);
        return true;
    }
    request_t *copy = cJSON_malloc(sizeof *copy);
    if (copy) *copy = r;
    if (!copy || !s_config.submit || !s_config.submit(execute, copy)) {
        cJSON_free(copy);
        atomic_store(&s_busy, false);
        send_refusal(&r, "queue_unavailable", NULL, -1);
    }
    return true;
}

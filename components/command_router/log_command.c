/*
 * log_command.c — log_list / log_read (contract: include/log_command.h).
 *
 * Two task contexts, deliberately split:
 *   esp-mqtt task  : log_command_handle() — parse, validate, copy a bounded
 *                    request, enqueue. No I/O, no SD, no event_log.
 *   maint worker   : execute() — maintenance lock → SD-state refusals →
 *                    sdcard_io_begin → opendir/fopen "rb"/fread → io_end →
 *                    maintenance_end → THEN publish. The reply is published
 *                    after every SD reference is released, so a slow MQTT
 *                    write can never hold up a low-battery unmount.
 */
#include "log_command.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static log_command_config_t s_config;
/* One log operation at a time: a QoS1 duplicate or a second operator gets
 * "busy" at once instead of queueing a second 8 KiB read behind the first. */
static atomic_bool s_busy;

typedef struct {
    char    id[65];
    char    name[LOG_NAME_MAX + 1];
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

/* ── replies ───────────────────────────────────────────────────────────── */

static cJSON *reply_base(const request_t *r, bool ok, const char *detail)
{
    cJSON *j = cJSON_CreateObject();
    if (j == NULL) return NULL;
    cJSON_AddStringToObject(j, "type", r->read ? "log_read_result" : "log_list_result");
    cJSON_AddStringToObject(j, "id", r->id);
    cJSON_AddBoolToObject(j, "ok", ok);
    cJSON_AddStringToObject(j, "detail", detail);
    if (r->read) {
        cJSON_AddStringToObject(j, "name", r->name);
    } else {
        cJSON_AddStringToObject(j, "dir", s_config.dir ? s_config.dir : "");
    }
    return j;
}

static char *print_and_free(cJSON *j)
{
    if (j == NULL) return NULL;
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}

static void send_and_free(char *json)
{
    if (json == NULL) return;
    if (s_config.reply) s_config.reply(json);
    cJSON_free(json);
}

static char *refusal(const request_t *r, const char *detail)
{
    return print_and_free(reply_base(r, false, detail));
}

/* ── SD work (maintenance worker, inside io_begin/io_end) ──────────────── */

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

static char *do_list(const request_t *r)
{
    DIR *d = opendir(s_config.dir);
    if (d == NULL) return refusal(r, "dir_unavailable");
    list_entry_t *e = calloc(LOG_LIST_MAX_FILES, sizeof *e);
    if (e == NULL) {
        closedir(d);
        return refusal(r, "no_mem");
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
    cJSON *j = reply_base(r, true, "ok");
    cJSON *files = j ? cJSON_AddArrayToObject(j, "files") : NULL;
    for (size_t i = 0; files != NULL && i < count; i++) {
        cJSON *f = cJSON_CreateObject();
        if (f == NULL) break;
        cJSON_AddStringToObject(f, "name", e[i].name);
        cJSON_AddNumberToObject(f, "size", e[i].size);
        cJSON_AddItemToArray(files, f);
    }
    if (j) cJSON_AddNumberToObject(j, "omitted", omitted);
    free(e);
    return print_and_free(j);
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

static char *do_read(const request_t *r)
{
    if (!found_in_dir(r->name)) return refusal(r, "not_found");
    char path[300];
    if (!join_path(path, sizeof path, r->name)) return refusal(r, "not_found");
    FILE *f = fopen(path, "rb");                 /* read-only, always */
    if (f == NULL) return refusal(r, "open_failed");
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return refusal(r, "stat_failed");
    }
    if (!S_ISREG(st.st_mode)) {
        fclose(f);
        return refusal(r, "not_a_file");
    }
    const int64_t size = (int64_t)st.st_size;
    int64_t start = 0, len = 0;
    const char *bad = log_command_resolve(size, r->offset, r->length, &start, &len);
    if (bad != NULL) {
        fclose(f);
        cJSON *j = reply_base(r, false, bad);
        if (j) cJSON_AddNumberToObject(j, "size", (double)size);
        return print_and_free(j);
    }
    uint8_t *raw = malloc(len > 0 ? (size_t)len : 1U);
    if (raw == NULL) {
        fclose(f);
        return refusal(r, "no_mem");
    }
    size_t got = 0;
    if (len > 0) {
        if (fseek(f, (long)start, SEEK_SET) != 0) {
            fclose(f);
            free(raw);
            return refusal(r, "seek_failed");
        }
        got = fread(raw, 1, (size_t)len, f);
        if (got < (size_t)len && ferror(f)) {
            fclose(f);
            free(raw);
            return refusal(r, "read_failed");
        }
    }
    fclose(f);

    cJSON *j = reply_base(r, true, "ok");
    if (j == NULL) {
        free(raw);
        return NULL;
    }
    cJSON_AddNumberToObject(j, "size", (double)size);
    cJSON_AddNumberToObject(j, "offset", (double)start);
    cJSON_AddNumberToObject(j, "length", (double)got);
    cJSON_AddBoolToObject(j, "eof", start + (int64_t)got >= size);
    cJSON_AddStringToObject(j, "encoding", "base64");
    char *head = print_and_free(j);
    if (head == NULL) {
        free(raw);
        return NULL;
    }
    /* Splice `,"data":"<base64>"}` onto cJSON's (correctly escaped) header
     * instead of handing cJSON an 11 KiB string it would copy twice more. */
    const size_t hl = strlen(head);
    const size_t b64 = 4 * ((got + 2) / 3);
    char *out = cJSON_malloc(hl + b64 + 16);
    if (out == NULL) {
        cJSON_free(head);
        free(raw);
        return refusal(r, "no_mem");
    }
    memcpy(out, head, hl - 1);                   /* drop the closing '}' */
    size_t o = hl - 1;
    memcpy(out + o, ",\"data\":\"", 9);
    o += 9;
    o += log_command_base64(raw, got, out + o, b64 + 1);
    memcpy(out + o, "\"}", 3);
    cJSON_free(head);
    free(raw);
    return out;
}

static void execute(void *arg)
{
    request_t *r = arg;
    char *json = NULL;
    if (!s_config.maintenance_begin || !s_config.maintenance_end || !s_config.maintenance_begin()) {
        /* An OTA / AMBIT flash / schedule install owns the device: never
         * compete with it for the card. */
        json = refusal(r, "maintenance_active");
    } else {
        /* Order matters: a parked card is also unmounted, and the operator
         * must see WHY (low battery), not just "absent". */
        const char *why = NULL;
        if (s_config.sd_parked && s_config.sd_parked())            why = "sd_parked";
        else if (!s_config.sd_mounted || !s_config.sd_mounted())   why = "sd_absent";
        else if (s_config.sd_io_lost && s_config.sd_io_lost())     why = "sd_io_lost";
        else if (!s_config.io_begin || !s_config.io_begin())       why = "sd_busy";
        if (why != NULL) {
            json = refusal(r, why);
        } else {
            json = r->read ? do_read(r) : do_list(r);
            s_config.io_end();
        }
        s_config.maintenance_end();
    }
    send_and_free(json);                          /* after every SD reference is gone */
    free(r);
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
        send_and_free(refusal(&r, "invalid_id"));
        return true;
    }
    memcpy(r.id, id->valuestring, strlen(id->valuestring) + 1);
    if (read) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (!cJSON_IsString(name) || !log_command_name_valid(name->valuestring)) {
            send_and_free(refusal(&r, "invalid_name"));
            return true;
        }
        memcpy(r.name, name->valuestring, strlen(name->valuestring) + 1);
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
        if (offset != NULL && !cJSON_IsNull(offset) && !json_int(offset, &r.offset)) {
            send_and_free(refusal(&r, "invalid_offset"));
            return true;
        }
        const cJSON *length = cJSON_GetObjectItemCaseSensitive(root, "length");
        if (length != NULL && !cJSON_IsNull(length) && (!json_int(length, &r.length) || r.length <= 0)) {
            send_and_free(refusal(&r, "invalid_length"));
            return true;
        }
        if (r.length > LOG_READ_MAX_LEN) r.length = LOG_READ_MAX_LEN;   /* clamp, reported */
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        send_and_free(refusal(&r, "busy"));
        return true;
    }
    request_t *copy = malloc(sizeof *copy);
    if (copy) *copy = r;
    if (!copy || !s_config.submit || !s_config.submit(execute, copy)) {
        free(copy);
        atomic_store(&s_busy, false);
        send_and_free(refusal(&r, "queue_unavailable"));
    }
    return true;
}

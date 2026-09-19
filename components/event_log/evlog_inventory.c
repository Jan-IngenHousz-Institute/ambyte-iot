#include "evlog_inventory.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Room for "<archive_dir>/arc-<int64>.log"; host tests pass long temp paths. */
#define EVLOG_INVENTORY_PATH_MAX 260

bool evlog_inventory_parse_name(const char *name, const char *prefix, int64_t *out_id,
                                uint32_t *out_suffix)
{
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return false;
    const char *p = name + plen;
    if (*p < '0' || *p > '9') return false;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p || v <= 0) return false;
    uint32_t suffix = 0;
    if (*end == '-') {                       /* arc-<id>-<seq>.log: collision-avoiding name */
        const char *q = end + 1;
        if (*q < '0' || *q > '9') return false;
        long long s = strtoll(q, &end, 10);
        if (end == q || s < 0) return false;
        suffix = (uint32_t)s;
    }
    if (strcmp(end, ".log") != 0) return false;
    if (out_id) *out_id = (int64_t)v;
    if (out_suffix) *out_suffix = suffix;
    return true;
}

static bool parse_named_log(const char *name, const char *prefix, int64_t *out_id)
{
    return evlog_inventory_parse_name(name, prefix, out_id, NULL);
}

typedef struct { int64_t id; uint32_t suffix; } arc_entry_t;

static int cmp_arc(const void *a, const void *b)
{
    const arc_entry_t *x = a, *y = b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return x->suffix < y->suffix ? -1 : (x->suffix > y->suffix ? 1 : 0);
}

static bool io_begin(const evlog_inventory_hooks_t *h)
{
    return (h == NULL || h->io_begin == NULL) ? true : h->io_begin();
}

static void io_end(const evlog_inventory_hooks_t *h)
{
    if (h != NULL && h->io_end != NULL) h->io_end();
}

/* Parse "<id>\t<channel>\t<device>\t<tag>\t<cmd_raw>\t<start_ms>\t..." from the
 * head of a line. cmd_raw is bounded at 543 B by the producer, so the sixth
 * field starts well inside EVLOG_INVENTORY_HEAD_BYTES for every record the
 * firmware writes; anything else counts as unparsed. */
bool evlog_inventory_parse_head(const char *head, size_t head_len, int64_t *out_id, int64_t *out_start_ms);

static bool parse_head(const char *head, size_t head_len, int64_t *out_id, int64_t *out_start_ms)
{
    return evlog_inventory_parse_head(head, head_len, out_id, out_start_ms);
}

bool evlog_inventory_parse_head(const char *head, size_t head_len, int64_t *out_id, int64_t *out_start_ms)
{
    if (head_len == 0 || head[0] < '0' || head[0] > '9') return false;
    char *end = NULL;
    long long id = strtoll(head, &end, 10);
    if (end == head || id <= 0 || *end != '\t') return false;
    const char *p = end;
    for (int tab = 1; tab < 5; tab++) {
        p = memchr(p + 1, '\t', head_len - (size_t)(p + 1 - head));
        if (p == NULL) return false;
    }
    p++;
    if ((size_t)(p - head) >= head_len || *p < '0' || *p > '9') return false;
    long long start_ms = strtoll(p, &end, 10);
    if (end == p || *end != '\t') return false;
    *out_id = (int64_t)id;
    *out_start_ms = (int64_t)start_ms;
    return true;
}

static bool in_window(const evlog_inventory_window_t *w, int64_t id, int64_t start_ms)
{
    if (w == NULL) return true;
    if (w->from_id && id < w->from_id) return false;
    if (w->to_id && id > w->to_id) return false;
    if (w->from_ms && start_ms < w->from_ms) return false;
    if (w->to_ms && start_ms > w->to_ms) return false;
    return true;
}

static void scan_file(const char *path, int64_t name_id, const evlog_inventory_window_t *win,
                      const evlog_inventory_hooks_t *hooks, char *buf, evlog_inventory_t *out)
{
    evlog_inventory_file_t row;
    memset(&row, 0, sizeof row);
    row.name_id = name_id;

    if (!io_begin(hooks)) { out->sd_lost = true; return; }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        out->open_failed++;
        io_end(hooks);
        return;
    }

    bool   line_open = false;      /* mid-line: a chunk without '\n' preceded this one */
    bool   head_ok   = false;
    int64_t id = 0, start_ms = 0;
    while (fgets(buf, EVLOG_INVENTORY_HEAD_BYTES, f) != NULL) {
        size_t n = strlen(buf);
        row.bytes += (uint32_t)n;
        bool eol = (n > 0 && buf[n - 1] == '\n');
        if (!line_open) {
            head_ok = parse_head(buf, n, &id, &start_ms);
        }
        if (!eol) { line_open = true; continue; }

        /* One complete record. */
        line_open = false;
        row.records++;
        if (!head_ok) { out->unparsed++; continue; }
        if (row.first_id == 0) row.first_id = id;
        row.last_id = id;
        if (out->min_id == 0 || id < out->min_id) out->min_id = id;
        if (id > out->max_id) out->max_id = id;
        if (out->min_start_ms == 0 || start_ms < out->min_start_ms) out->min_start_ms = start_ms;
        if (start_ms > out->max_start_ms) out->max_start_ms = start_ms;
        if (start_ms < EVLOG_INVENTORY_CLOCK_FLOOR_MS) out->pre_clock_floor++;
        if (in_window(win, id, start_ms)) row.in_window++;
    }
    if (line_open) out->torn++;
    fclose(f);
    io_end(hooks);

    out->records   += row.records;
    out->in_window += row.in_window;
    out->bytes     += row.bytes;
    if (out->listed < EVLOG_INVENTORY_LIST_MAX) {
        out->list[out->listed++] = row;
    } else {
        out->list_truncated = true;
    }
}

static uint32_t count_named(const char *dir, const char *prefix, const evlog_inventory_hooks_t *hooks,
                            bool *out_present, bool *out_lost)
{
    uint32_t n = 0;
    if (dir == NULL) { if (out_present) *out_present = false; return 0; }
    if (!io_begin(hooks)) { if (out_lost) *out_lost = true; return 0; }
    DIR *d = opendir(dir);
    if (out_present) *out_present = (d != NULL);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (parse_named_log(ent->d_name, prefix, NULL)) n++;
        }
        closedir(d);
    }
    io_end(hooks);
    return n;
}

esp_err_t evlog_inventory_scan(const char *archive_dir, const char *legacy_dir,
                               const evlog_inventory_window_t *win,
                               const evlog_inventory_hooks_t *hooks,
                               evlog_inventory_t *out)
{
    if (archive_dir == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    arc_entry_t *ids = calloc(EVLOG_INVENTORY_MAX_FILES, sizeof *ids);
    char *buf = malloc(EVLOG_INVENTORY_HEAD_BYTES);
    if (ids == NULL || buf == NULL) { free(ids); free(buf); return ESP_ERR_NO_MEM; }

    uint32_t nids = 0;
    if (!io_begin(hooks)) { out->sd_lost = true; free(ids); free(buf); return ESP_OK; }
    DIR *d = opendir(archive_dir);
    out->archive_dir_present = (d != NULL);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            arc_entry_t e;
            if (!evlog_inventory_parse_name(ent->d_name, "arc-", &e.id, &e.suffix)) { out->other_entries++; continue; }
            out->files++;
            if (nids < EVLOG_INVENTORY_MAX_FILES) ids[nids++] = e;
            else out->files_truncated = true;
        }
        closedir(d);
    }
    io_end(hooks);

    qsort(ids, nids, sizeof *ids, cmp_arc);
    char path[EVLOG_INVENTORY_PATH_MAX];
    for (uint32_t i = 0; i < nids && !out->sd_lost; i++) {
        if (ids[i].suffix) {
            snprintf(path, sizeof path, "%s/arc-%lld-%u.log", archive_dir, (long long)ids[i].id, (unsigned)ids[i].suffix);
        } else {
            snprintf(path, sizeof path, "%s/arc-%lld.log", archive_dir, (long long)ids[i].id);
        }
        scan_file(path, ids[i].id, win, hooks, buf, out);
        if (hooks != NULL && hooks->yield != NULL) hooks->yield();
    }

    if (!out->sd_lost) {
        out->legacy_files = count_named(legacy_dir, "ev-", hooks, NULL, &out->sd_lost);
    }

    free(ids);
    free(buf);
    return ESP_OK;
}

/* Copy `s` into `dst` as a JSON string body: escape quote and backslash, drop
 * control characters, stop at `max_chars` source characters. */
static int json_copy(char *dst, size_t cap, const char *s, size_t max_chars)
{
    size_t o = 0;
    if (s == NULL) s = "";
    for (size_t i = 0; s[i] != '\0' && i < max_chars; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) return -1;
            dst[o++] = '\\';
        } else if (o + 1 >= cap) {
            return -1;
        }
        dst[o++] = (char)c;
    }
    if (o >= cap) return -1;
    dst[o] = '\0';
    return (int)o;
}

#define APPEND(...)                                                              \
    do {                                                                         \
        int _n = snprintf(buf + o, cap - o, __VA_ARGS__);                        \
        if (_n < 0 || (size_t)_n >= cap - o) return -1;                          \
        o += (size_t)_n;                                                         \
    } while (0)

int evlog_inventory_render_json(const evlog_inventory_t *inv,
                                const evlog_inventory_window_t *win,
                                const char *id, const char *device_id, const char *firmware,
                                bool sd_mounted, uint64_t store_free_bytes,
                                int64_t pending, int64_t last_acked_id, int64_t next_id,
                                uint32_t scan_ms, bool include_list,
                                char *buf, size_t cap)
{
    if (inv == NULL || buf == NULL || cap == 0) return -1;
    char sid[130], sdev[130], sfw[66];
    if (json_copy(sid, sizeof sid, id, 64) < 0 || json_copy(sdev, sizeof sdev, device_id, 64) < 0 ||
        json_copy(sfw, sizeof sfw, firmware, 32) < 0) {
        return -1;
    }
    size_t o = 0;
    APPEND("{\"type\":\"evlog_inventory\",\"id\":\"%s\",\"device_id\":\"%s\",\"fw\":\"%s\",\"ok\":true,"
           "\"sd_mounted\":%s,\"scan_ms\":%" PRIu32 ",",
           sid, sdev, sfw, sd_mounted ? "true" : "false", scan_ms);
    APPEND("\"window\":{\"from_id\":%lld,\"to_id\":%lld,\"from_ms\":%lld,\"to_ms\":%lld},",
           (long long)(win ? win->from_id : 0), (long long)(win ? win->to_id : 0),
           (long long)(win ? win->from_ms : 0), (long long)(win ? win->to_ms : 0));
    APPEND("\"archive\":{\"present\":%s,\"sd_lost\":%s,\"files\":%" PRIu32 ",\"other_entries\":%" PRIu32
           ",\"open_failed\":%" PRIu32 ",\"records\":%" PRIu32 ",\"torn\":%" PRIu32 ",\"unparsed\":%" PRIu32
           ",\"in_window\":%" PRIu32 ",\"pre_2024\":%" PRIu32 ",\"bytes\":%llu,"
           "\"min_id\":%lld,\"max_id\":%lld,\"min_start_ms\":%lld,\"max_start_ms\":%lld,"
           "\"files_truncated\":%s},",
           inv->archive_dir_present ? "true" : "false", inv->sd_lost ? "true" : "false",
           inv->files, inv->other_entries, inv->open_failed, inv->records, inv->torn, inv->unparsed,
           inv->in_window, inv->pre_clock_floor, (unsigned long long)inv->bytes,
           (long long)inv->min_id, (long long)inv->max_id,
           (long long)inv->min_start_ms, (long long)inv->max_start_ms,
           inv->files_truncated ? "true" : "false");
    APPEND("\"legacy_events_files\":%" PRIu32 ",", inv->legacy_files);
    APPEND("\"store\":{\"free_bytes\":%llu,\"pending\":%lld,\"last_acked_id\":%lld,\"next_id\":%lld}",
           (unsigned long long)store_free_bytes, (long long)pending, (long long)last_acked_id,
           (long long)next_id);
    if (include_list) {
        APPEND(",\"list_truncated\":%s,\"files_list\":[", inv->list_truncated ? "true" : "false");
        for (uint32_t i = 0; i < inv->listed; i++) {
            const evlog_inventory_file_t *r = &inv->list[i];
            APPEND("%s{\"name_id\":%lld,\"first_id\":%lld,\"last_id\":%lld,\"records\":%" PRIu32
                   ",\"in_window\":%" PRIu32 ",\"bytes\":%" PRIu32 "}",
                   i ? "," : "", (long long)r->name_id, (long long)r->first_id, (long long)r->last_id,
                   r->records, r->in_window, r->bytes);
        }
        APPEND("]");
    }
    APPEND("}");
    return (int)o;
}

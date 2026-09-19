#include "evlog_replay.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "event_log.h"
#include "evlog_inventory.h"
#include "sd_card.h"

#define TAG "evlog_replay"

#define NVS_NS        "evreplay"
#define KEY_TOKEN     "tok"
#define KEY_FROM_ID   "fid"
#define KEY_TO_ID     "tid"
#define KEY_FROM_MS   "fms"
#define KEY_TO_MS     "tms"
#define KEY_CHUNK     "chunk"
#define KEY_NEXT      "next"
#define KEY_STATE     "state"
#define KEY_APPENDED  "app"
#define KEY_HIST      "hist"

#define REPLAY_TASK_STACK     8192
#define REPLAY_LINE_CAP       (65552u + 64u)     /* EVLOG_RECORD_CAP_NORMAL + slack; PSRAM */
#define REPLAY_MAX_FILES      512
#define REPLAY_PAUSE_MS       30000
#define REPLAY_BOOT_DELAY_MS  30000
#define REPLAY_PROGRESS_EVERY 1000
#define REPLAY_CMD_ID_MAX     64

typedef struct {
    evlog_replay_req_t   req;
    evlog_replay_state_t state;
    int64_t              next_id;     /* first id not yet appended */
    uint32_t             appended;
} replay_job_t;

typedef struct { int64_t from, to; } id_range_t;

typedef enum { OP_COUNT = 0, OP_RUN } replay_op_t;

typedef struct {
    replay_op_t        op;
    evlog_replay_req_t req;
    char               cmd_id[REPLAY_CMD_ID_MAX + 1];
} replay_task_arg_t;

static evlog_replay_config_t s_cfg;
static bool        s_ready = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static replay_job_t s_job;                              /* guarded by s_mux */
static id_range_t   s_hist[EVLOG_REPLAY_HISTORY];       /* guarded by s_mux */
static volatile bool s_task_busy = false;
static volatile bool s_cancel    = false;

/* ── persistence ─────────────────────────────────────────────────────── */

static void job_load(void)
{
    nvs_handle_t h;
    replay_job_t j;
    id_range_t hist[EVLOG_REPLAY_HISTORY];
    memset(&j, 0, sizeof j);
    memset(hist, 0, sizeof hist);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof j.req.token;
        (void)nvs_get_str(h, KEY_TOKEN, j.req.token, &len);
        (void)nvs_get_i64(h, KEY_FROM_ID, &j.req.from_id);
        (void)nvs_get_i64(h, KEY_TO_ID, &j.req.to_id);
        (void)nvs_get_i64(h, KEY_FROM_MS, &j.req.from_ms);
        (void)nvs_get_i64(h, KEY_TO_MS, &j.req.to_ms);
        (void)nvs_get_u32(h, KEY_CHUNK, &j.req.chunk);
        (void)nvs_get_i64(h, KEY_NEXT, &j.next_id);
        uint8_t st = 0;
        (void)nvs_get_u8(h, KEY_STATE, &st);
        j.state = (evlog_replay_state_t)st;
        (void)nvs_get_u32(h, KEY_APPENDED, &j.appended);
        size_t hl = sizeof hist;
        (void)nvs_get_blob(h, KEY_HIST, hist, &hl);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    s_job = j;
    memcpy(s_hist, hist, sizeof hist);
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t job_save(const replay_job_t *j)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    (void)nvs_set_str(h, KEY_TOKEN, j->req.token);
    (void)nvs_set_i64(h, KEY_FROM_ID, j->req.from_id);
    (void)nvs_set_i64(h, KEY_TO_ID, j->req.to_id);
    (void)nvs_set_i64(h, KEY_FROM_MS, j->req.from_ms);
    (void)nvs_set_i64(h, KEY_TO_MS, j->req.to_ms);
    (void)nvs_set_u32(h, KEY_CHUNK, j->req.chunk);
    (void)nvs_set_i64(h, KEY_NEXT, j->next_id);
    (void)nvs_set_u8(h, KEY_STATE, (uint8_t)j->state);
    (void)nvs_set_u32(h, KEY_APPENDED, j->appended);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void progress_save(int64_t next_id, uint32_t appended)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_i64(h, KEY_NEXT, next_id);
    (void)nvs_set_u32(h, KEY_APPENDED, appended);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void hist_push_and_save(int64_t from, int64_t to)
{
    id_range_t hist[EVLOG_REPLAY_HISTORY];
    portENTER_CRITICAL(&s_mux);
    memmove(&s_hist[1], &s_hist[0], sizeof(id_range_t) * (EVLOG_REPLAY_HISTORY - 1));
    s_hist[0].from = from;
    s_hist[0].to   = to;
    memcpy(hist, s_hist, sizeof hist);
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_blob(h, KEY_HIST, hist, sizeof hist);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

/* ── replies ─────────────────────────────────────────────────────────── */

static bool token_ok(const char *s, size_t max)
{
    if (s == NULL || s[0] == '\0') return false;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok || n >= max) return false;
    }
    return true;
}

static const char *state_name(evlog_replay_state_t s)
{
    switch (s) {
    case EVLOG_REPLAY_RUNNING:   return "running";
    case EVLOG_REPLAY_DONE:      return "done";
    case EVLOG_REPLAY_CANCELLED: return "cancelled";
    case EVLOG_REPLAY_FAILED:    return "failed";
    default:                     return "idle";
    }
}

/* All strings interpolated here are validated by token_ok() (token, cmd_id) or
 * firmware-owned, so no JSON escaping is needed. */
static void reply(const char *cmd_id, const char *mode, bool ok, const replay_job_t *j,
                  const char *detail, const char *extra_json)
{
    if (s_cfg.publish == NULL || s_cfg.status_topic == NULL || s_cfg.status_topic[0] == '\0') return;
    char buf[640];
    int n = snprintf(buf, sizeof buf,
        "{\"type\":\"evlog_replay_result\",\"id\":\"%.64s\",\"device_id\":\"%s\",\"fw\":\"%s\","
        "\"mode\":\"%s\",\"ok\":%s,\"state\":\"%s\",\"token\":\"%s\","
        "\"from_id\":%lld,\"to_id\":%lld,\"from_ms\":%lld,\"to_ms\":%lld,\"chunk\":%u,"
        "\"next_id\":%lld,\"appended\":%u,\"detail\":\"%.96s\"%s%s}",
        cmd_id ? cmd_id : "", s_cfg.device_id ? s_cfg.device_id : "",
        s_cfg.firmware_version ? s_cfg.firmware_version : "",
        mode, ok ? "true" : "false", state_name(j ? j->state : EVLOG_REPLAY_IDLE),
        j ? j->req.token : "",
        (long long)(j ? j->req.from_id : 0), (long long)(j ? j->req.to_id : 0),
        (long long)(j ? j->req.from_ms : 0), (long long)(j ? j->req.to_ms : 0),
        (unsigned)(j ? j->req.chunk : 0), (long long)(j ? j->next_id : 0),
        (unsigned)(j ? j->appended : 0), detail ? detail : "",
        extra_json ? "," : "", extra_json ? extra_json : "");
    if (n <= 0 || (size_t)n >= sizeof buf) return;
    int msg_id = 0;
    s_cfg.publish(s_cfg.status_topic, buf, (size_t)n, &msg_id);
}

/* ── archive iteration ───────────────────────────────────────────────── */

typedef struct { int64_t name_id; uint32_t suffix; } arc_name_t;

static int cmp_arc(const void *a, const void *b)
{
    const arc_name_t *x = a, *y = b;
    if (x->name_id != y->name_id) return x->name_id < y->name_id ? -1 : 1;
    return x->suffix < y->suffix ? -1 : (x->suffix > y->suffix ? 1 : 0);
}

static bool list_archive(arc_name_t *names, uint32_t cap, uint32_t *out_n, bool *truncated)
{
    *out_n = 0;
    *truncated = false;
    if (!sdcard_io_begin()) return false;
    DIR *d = opendir(EVLOG_INVENTORY_ARCHIVE_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            arc_name_t a;
            if (!evlog_inventory_parse_name(ent->d_name, "arc-", &a.name_id, &a.suffix)) continue;
            if (*out_n < cap) names[(*out_n)++] = a;
            else *truncated = true;
        }
        closedir(d);
    }
    sdcard_io_end();
    qsort(names, *out_n, sizeof *names, cmp_arc);
    return true;
}

static void arc_path(char *buf, size_t cap, const arc_name_t *a)
{
    if (a->suffix) snprintf(buf, cap, "%s/arc-%lld-%u.log", EVLOG_INVENTORY_ARCHIVE_DIR, (long long)a->name_id, (unsigned)a->suffix);
    else           snprintf(buf, cap, "%s/arc-%lld.log", EVLOG_INVENTORY_ARCHIVE_DIR, (long long)a->name_id);
}

static bool in_window(const evlog_replay_req_t *r, int64_t id, int64_t start_ms)
{
    if (r->from_id && id < r->from_id) return false;
    if (r->to_id && id > r->to_id) return false;
    if (r->from_ms && start_ms < r->from_ms) return false;
    if (r->to_ms && start_ms > r->to_ms) return false;
    return true;
}

/* Wait until the store can take more records or the job is cancelled. */
static bool wait_for_room(void)
{
    for (;;) {
        if (s_cancel) return false;
        uint64_t freeb = 0;
        evlog_health_t h;
        memset(&h, 0, sizeof h);
        (void)event_log_free_bytes(&freeb);
        (void)event_log_health(&h);
        if (freeb >= EVLOG_REPLAY_MIN_FREE_BYTES && h.pending <= EVLOG_REPLAY_PENDING_CAP) return true;
        ESP_LOGI(TAG, "store busy (free=%llu B, pending=%lld) — replay paused %u s",
                 (unsigned long long)freeb, (long long)h.pending, (unsigned)(REPLAY_PAUSE_MS / 1000));
        vTaskDelay(pdMS_TO_TICKS(REPLAY_PAUSE_MS));
    }
}

/* ── the worker ──────────────────────────────────────────────────────── */

static void do_count(const replay_task_arg_t *a)
{
    evlog_inventory_window_t win = {
        .from_id = a->req.from_id, .to_id = a->req.to_id, .from_ms = a->req.from_ms, .to_ms = a->req.to_ms,
    };
    evlog_inventory_hooks_t hooks = { .io_begin = sdcard_io_begin, .io_end = sdcard_io_end, .yield = NULL };
    evlog_inventory_t *inv = calloc(1, sizeof *inv);
    replay_job_t probe;
    memset(&probe, 0, sizeof probe);
    probe.req = a->req;
    if (inv == NULL) { reply(a->cmd_id, "count", false, &probe, "no_mem", NULL); return; }
    if (!sdcard_is_mounted()) { reply(a->cmd_id, "count", false, &probe, "sd_not_mounted", NULL); free(inv); return; }
    esp_err_t err = evlog_inventory_scan(EVLOG_INVENTORY_ARCHIVE_DIR, NULL, &win, &hooks, inv);
    char extra[224];
    snprintf(extra, sizeof extra,
             "\"matched\":%" PRIu32 ",\"files\":%" PRIu32 ",\"records\":%" PRIu32 ",\"unparsed\":%" PRIu32
             ",\"torn\":%" PRIu32 ",\"pre_2024\":%" PRIu32 ",\"sd_lost\":%s,\"archive_present\":%s",
             inv->in_window, inv->files, inv->records, inv->unparsed, inv->torn, inv->pre_clock_floor,
             inv->sd_lost ? "true" : "false", inv->archive_dir_present ? "true" : "false");
    reply(a->cmd_id, "count", err == ESP_OK && !inv->sd_lost, &probe,
          err == ESP_OK ? (inv->sd_lost ? "sd_lost" : "ok") : esp_err_to_name(err), extra);
    free(inv);
}

static void finish(replay_job_t *j, evlog_replay_state_t st, const char *cmd_id, const char *detail,
                   uint32_t skipped_window, uint32_t skipped_unparsed, bool resume_exact)
{
    (void)event_log_flush();
    j->state = st;
    (void)job_save(j);
    portENTER_CRITICAL(&s_mux);
    s_job = *j;
    portEXIT_CRITICAL(&s_mux);
    if (st == EVLOG_REPLAY_DONE) hist_push_and_save(j->req.from_id, j->req.to_id);
    if (s_cfg.notify_publisher) s_cfg.notify_publisher();
    char extra[128];
    snprintf(extra, sizeof extra, "\"skipped_window\":%" PRIu32 ",\"skipped_unparsed\":%" PRIu32 ",\"resume_exact\":%s",
             skipped_window, skipped_unparsed, resume_exact ? "true" : "false");
    reply(cmd_id, "run", st == EVLOG_REPLAY_DONE, j, detail, extra);
    ESP_LOGW(TAG, "replay %s: %s (appended=%u next_id=%lld)", j->req.token, state_name(st),
             (unsigned)j->appended, (long long)j->next_id);
}

static void do_run(const replay_task_arg_t *a)
{
    replay_job_t j;
    portENTER_CRITICAL(&s_mux);
    j = s_job;
    portEXIT_CRITICAL(&s_mux);
    uint32_t chunk = j.req.chunk ? j.req.chunk : EVLOG_REPLAY_DEFAULT_CHUNK;
    uint32_t skipped_window = 0, skipped_unparsed = 0;
    bool resume_exact = true;

    /* Exact resume: whatever a previous attempt appended is still PENDING in
     * the store (it was fsync'd before its progress was committed, or it was
     * lost with the tail, either way never skipped). Continue after the
     * highest such id so a reset between append and progress commit cannot
     * duplicate a chunk. */
    if (j.next_id > j.req.from_id || j.appended > 0) {
        int64_t mx = 0;
        bool capped = false;
        if (event_log_max_pending_id_in_range(j.req.from_id, j.req.to_id, &mx, &capped) == ESP_OK) {
            if (mx + 1 > j.next_id) j.next_id = mx + 1;
            resume_exact = !capped;
        } else {
            resume_exact = false;
        }
    }
    if (j.next_id < j.req.from_id) j.next_id = j.req.from_id;

    char *line = malloc(REPLAY_LINE_CAP);
    arc_name_t *names = calloc(REPLAY_MAX_FILES, sizeof *names);
    if (line == NULL || names == NULL) {
        free(line); free(names);
        finish(&j, EVLOG_REPLAY_FAILED, a->cmd_id, "no_mem", 0, 0, resume_exact);
        return;
    }
    if (!sdcard_is_mounted()) {
        free(line); free(names);
        finish(&j, EVLOG_REPLAY_FAILED, a->cmd_id, "sd_not_mounted", 0, 0, resume_exact);
        return;
    }
    uint32_t nfiles = 0;
    bool truncated = false;
    if (!list_archive(names, REPLAY_MAX_FILES, &nfiles, &truncated)) {
        free(line); free(names);
        finish(&j, EVLOG_REPLAY_FAILED, a->cmd_id, "sd_lost", 0, 0, resume_exact);
        return;
    }
    if (truncated) ESP_LOGW(TAG, "more than %u archive files — later files not replayed", (unsigned)REPLAY_MAX_FILES);

    uint32_t in_chunk = 0, since_progress = 0;
    bool failed = false;
    const char *fail_detail = "";

    for (uint32_t i = 0; i < nfiles && !failed && !s_cancel; i++) {
        /* Files are named by their first id and sorted. Skip files that cannot
         * contain anything >= next_id (the next file starts at or below it) or
         * anything <= to_id. */
        if (i + 1 < nfiles && names[i + 1].name_id <= j.next_id) continue;
        if (j.req.to_id && names[i].name_id > j.req.to_id) break;

        char path[96];
        arc_path(path, sizeof path, &names[i]);
        bool file_done = false;
        while (!file_done && !failed && !s_cancel) {
            if (!sdcard_io_begin()) { failed = true; fail_detail = "sd_lost"; break; }
            FILE *f = fopen(path, "rb");
            if (f == NULL) { sdcard_io_end(); file_done = true; break; }   /* archived away meanwhile */
            bool paused = false;
            while (fgets(line, (int)REPLAY_LINE_CAP, f) != NULL) {
                size_t len = strlen(line);
                if (len == 0 || line[len - 1] != '\n') {
                    /* Over-long (cannot be a valid record) or torn tail: drain
                     * to the next newline and count it. */
                    int c;
                    while ((c = fgetc(f)) != EOF && c != '\n') { }
                    skipped_unparsed++;
                    continue;
                }
                int64_t id = 0, start_ms = 0;
                size_t head = len < 1024 ? len : 1024;
                if (!evlog_inventory_parse_head(line, head, &id, &start_ms)) { skipped_unparsed++; continue; }
                if (id < j.next_id) continue;                 /* already appended (resume) */
                if (!in_window(&j.req, id, start_ms)) { skipped_window++; continue; }

                esp_err_t err = event_log_append_verbatim(line, len, NULL);
                if (err == ESP_ERR_NO_MEM) {
                    /* Store full or too far ahead of the publisher: persist
                     * progress, release the SD gate and wait; this file is
                     * reopened and ids < next_id skipped. */
                    (void)event_log_flush();
                    progress_save(j.next_id, j.appended);
                    paused = true;
                    break;
                }
                if (err == ESP_ERR_INVALID_ARG) { skipped_unparsed++; continue; }
                if (err != ESP_OK) { failed = true; fail_detail = esp_err_to_name(err); break; }

                j.appended++;
                j.next_id = id + 1;
                if (++in_chunk >= chunk) {
                    (void)event_log_flush();
                    progress_save(j.next_id, j.appended);
                    if (s_cfg.notify_publisher) s_cfg.notify_publisher();
                    in_chunk = 0;
                    vTaskDelay(1);
                }
                if (++since_progress >= REPLAY_PROGRESS_EVERY) {
                    since_progress = 0;
                    reply(a->cmd_id, "run", true, &j, "progress", NULL);
                }
                if (s_cancel) break;
                /* Keep the store guard between records too: a live burst can fill
                 * the store while a chunk is in progress. */
                evlog_health_t h;
                if (event_log_health(&h) == ESP_OK && h.pending > EVLOG_REPLAY_PENDING_CAP) {
                    (void)event_log_flush();
                    progress_save(j.next_id, j.appended);
                    paused = true;
                    break;
                }
            }
            fclose(f);
            sdcard_io_end();
            if (paused) {
                if (!wait_for_room()) break;          /* cancelled while waiting */
                continue;                             /* reopen this file */
            }
            file_done = true;
        }
        vTaskDelay(1);
    }

    free(line);
    free(names);
    if (failed) {
        progress_save(j.next_id, j.appended);
        finish(&j, EVLOG_REPLAY_FAILED, a->cmd_id, fail_detail, skipped_window, skipped_unparsed, resume_exact);
    } else if (s_cancel) {
        finish(&j, EVLOG_REPLAY_CANCELLED, a->cmd_id, "cancelled", skipped_window, skipped_unparsed, resume_exact);
    } else {
        if (j.req.to_id) j.next_id = j.req.to_id + 1;
        finish(&j, EVLOG_REPLAY_DONE, a->cmd_id, "done", skipped_window, skipped_unparsed, resume_exact);
    }
}

static void replay_task(void *arg)
{
    replay_task_arg_t *a = arg;
    if (a->op == OP_COUNT) do_count(a);
    else                   do_run(a);
    free(a);
    s_task_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t spawn(replay_op_t op, const evlog_replay_req_t *req, const char *cmd_id)
{
    if (s_task_busy) return ESP_ERR_INVALID_STATE;
    replay_task_arg_t *a = calloc(1, sizeof *a);
    if (a == NULL) return ESP_ERR_NO_MEM;
    a->op = op;
    if (req) a->req = *req;
    if (cmd_id) strncpy(a->cmd_id, cmd_id, sizeof a->cmd_id - 1);
    s_task_busy = true;
    if (xTaskCreate(replay_task, "evlog_replay", REPLAY_TASK_STACK, a, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        s_task_busy = false;
        free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void boot_resume_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(REPLAY_BOOT_DELAY_MS));
    replay_job_t j;
    portENTER_CRITICAL(&s_mux);
    j = s_job;
    portEXIT_CRITICAL(&s_mux);
    if (j.state == EVLOG_REPLAY_RUNNING) {
        ESP_LOGW(TAG, "resuming replay %s at id %lld after reboot", j.req.token, (long long)j.next_id);
        (void)spawn(OP_RUN, &j.req, "boot-resume");
    }
    vTaskDelete(NULL);
}

/* ── public API ──────────────────────────────────────────────────────── */

esp_err_t evlog_replay_init(const evlog_replay_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    job_load();
    s_ready = true;
    replay_job_t j;
    portENTER_CRITICAL(&s_mux);
    j = s_job;
    portEXIT_CRITICAL(&s_mux);
    if (j.state == EVLOG_REPLAY_RUNNING) {
        if (xTaskCreate(boot_resume_task, "evlog_replay_bt", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            ESP_LOGW(TAG, "replay %s pending resume, but the resume task could not start", j.req.token);
        }
    }
    ESP_LOGI(TAG, "ready (job %s: %s)", j.req.token[0] ? j.req.token : "-", state_name(j.state));
    return ESP_OK;
}

static bool req_ok(const evlog_replay_req_t *r)
{
    if (r == NULL || !token_ok(r->token, EVLOG_REPLAY_TOKEN_MAX)) return false;
    if (r->from_id < 0 || r->to_id < 0 || r->from_ms < 0 || r->to_ms < 0) return false;
    if (r->to_id && r->from_id > r->to_id) return false;
    if (r->to_ms && r->from_ms > r->to_ms) return false;
    if (r->chunk > EVLOG_REPLAY_MAX_CHUNK) return false;
    return true;
}

esp_err_t evlog_replay_count(const evlog_replay_req_t *req, const char *cmd_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (req == NULL || (cmd_id && !token_ok(cmd_id, REPLAY_CMD_ID_MAX))) return ESP_ERR_INVALID_ARG;
    evlog_replay_req_t r = *req;
    if (!r.token[0]) strcpy(r.token, "count");
    if (!req_ok(&r)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = spawn(OP_COUNT, &r, cmd_id);
    if (err == ESP_ERR_INVALID_STATE) {
        replay_job_t probe; memset(&probe, 0, sizeof probe); probe.req = r;
        reply(cmd_id, "count", false, &probe, "busy", NULL);
    }
    return err;
}

esp_err_t evlog_replay_run(const evlog_replay_req_t *req, const char *cmd_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!req_ok(req) || (cmd_id && !token_ok(cmd_id, REPLAY_CMD_ID_MAX))) return ESP_ERR_INVALID_ARG;
    if (req->from_id == 0 && req->to_id == 0 && req->from_ms == 0 && req->to_ms == 0) {
        return ESP_ERR_INVALID_ARG;       /* refuse an unbounded "replay everything" */
    }

    replay_job_t cur;
    portENTER_CRITICAL(&s_mux);
    cur = s_job;
    portEXIT_CRITICAL(&s_mux);

    if (cur.req.token[0] && strcmp(cur.req.token, req->token) == 0) {
        if (cur.state == EVLOG_REPLAY_RUNNING) { reply(cmd_id, "run", true, &cur, s_task_busy ? "already_running" : "resuming", NULL); if (!s_task_busy) return spawn(OP_RUN, &cur.req, cmd_id); return ESP_OK; }
        if (cur.state == EVLOG_REPLAY_DONE)    { reply(cmd_id, "run", true, &cur, "already_done", NULL); return ESP_OK; }
        /* cancelled/failed with the same token: resume where it stopped */
        cur.state = EVLOG_REPLAY_RUNNING;
        s_cancel = false;
        (void)job_save(&cur);
        portENTER_CRITICAL(&s_mux); s_job = cur; portEXIT_CRITICAL(&s_mux);
        reply(cmd_id, "run", true, &cur, "resuming", NULL);
        return spawn(OP_RUN, &cur.req, cmd_id);
    }
    if (cur.state == EVLOG_REPLAY_RUNNING || s_task_busy) {
        reply(cmd_id, "run", false, &cur, "busy", NULL);
        return ESP_ERR_INVALID_STATE;
    }

    replay_job_t j;
    memset(&j, 0, sizeof j);
    j.req = *req;
    if (j.req.chunk == 0) j.req.chunk = EVLOG_REPLAY_DEFAULT_CHUNK;
    j.state = EVLOG_REPLAY_RUNNING;
    j.next_id = j.req.from_id;
    s_cancel = false;
    esp_err_t err = job_save(&j);
    if (err != ESP_OK) { reply(cmd_id, "run", false, &j, "nvs_error", NULL); return err; }
    portENTER_CRITICAL(&s_mux); s_job = j; portEXIT_CRITICAL(&s_mux);
    reply(cmd_id, "run", true, &j, "accepted", NULL);
    return spawn(OP_RUN, &j.req, cmd_id);
}

esp_err_t evlog_replay_cancel(const char *token, const char *cmd_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!token_ok(token, EVLOG_REPLAY_TOKEN_MAX) || (cmd_id && !token_ok(cmd_id, REPLAY_CMD_ID_MAX))) return ESP_ERR_INVALID_ARG;
    replay_job_t cur;
    portENTER_CRITICAL(&s_mux);
    cur = s_job;
    portEXIT_CRITICAL(&s_mux);
    if (strcmp(cur.req.token, token) != 0) { reply(cmd_id, "cancel", false, &cur, "unknown_token", NULL); return ESP_ERR_NOT_FOUND; }
    if (cur.state != EVLOG_REPLAY_RUNNING) { reply(cmd_id, "cancel", true, &cur, "not_running", NULL); return ESP_OK; }
    s_cancel = true;
    if (!s_task_busy) {
        /* Job persisted as RUNNING but no task alive (e.g. before boot resume). */
        cur.state = EVLOG_REPLAY_CANCELLED;
        (void)job_save(&cur);
        portENTER_CRITICAL(&s_mux); s_job = cur; portEXIT_CRITICAL(&s_mux);
        reply(cmd_id, "cancel", true, &cur, "cancelled", NULL);
    } else {
        reply(cmd_id, "cancel", true, &cur, "cancelling", NULL);   /* worker replies when it stops */
    }
    return ESP_OK;
}

esp_err_t evlog_replay_status(const char *cmd_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (cmd_id && !token_ok(cmd_id, REPLAY_CMD_ID_MAX)) return ESP_ERR_INVALID_ARG;
    replay_job_t cur;
    portENTER_CRITICAL(&s_mux);
    cur = s_job;
    portEXIT_CRITICAL(&s_mux);
    char extra[64];
    snprintf(extra, sizeof extra, "\"worker_active\":%s", s_task_busy ? "true" : "false");
    reply(cmd_id, "status", true, &cur, "ok", extra);
    return ESP_OK;
}

bool evlog_replay_covers(int64_t measure_id)
{
    bool hit = false;
    portENTER_CRITICAL(&s_mux);
    if (s_job.state != EVLOG_REPLAY_IDLE && s_job.req.token[0] &&
        measure_id >= s_job.req.from_id && (s_job.req.to_id == 0 || measure_id <= s_job.req.to_id)) {
        hit = true;
    }
    for (int i = 0; !hit && i < EVLOG_REPLAY_HISTORY; i++) {
        if (s_hist[i].to == 0 && s_hist[i].from == 0) continue;
        if (measure_id >= s_hist[i].from && (s_hist[i].to == 0 || measure_id <= s_hist[i].to)) hit = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return hit;
}

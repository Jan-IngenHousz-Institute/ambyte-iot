/*
 * event_log.c — append-only event log behind persistence_port.h.
 *
 * Replaces SQLite (which corrupted FATFS/SD via in-place page + header rewrites)
 * with an append-only store-and-forward FIFO. See event_log.h and
 * docs/append-log-persistence-plan.md for the design rationale; the Step-0 spike
 * proved this write pattern is corruption-free on the field card.
 *
 * Concurrency: every public op runs under s_mtx, serialising the Lua task
 * (store), the sync runner / MQTT ack task (claim/mark), and the CLI (stats).
 * Only one event is ever in flight at a time (device_commands enforces it), so a
 * single in-RAM "inflight" slot is sufficient.
 */

#include "event_log.h"
#include "sd_card.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "event_log"

#define EVLOG_DIR            "/sdcard/events"
#define EVLOG_LINE_CAP       12288            /* max bytes read back per record (incl '\n') */
#define EVLOG_MAX_RECORD     (EVLOG_LINE_CAP - 16)  /* store rejects bigger — must stay readable */
#define EVLOG_ROTATE_BYTES   (256 * 1024)     /* roll the tail file past this size */
#define EVLOG_FLUSH_PERIOD_MS 1500            /* periodic flush (NOT per-event fsync) */
#define EVLOG_FLUSH_EVERY_N  8                /* …or every N records, whichever first */
#define EVLOG_CURSOR_BATCH   16               /* persist read cursor every N acks */
#define EVLOG_ID_BLOCK       64               /* reserve next_id in blocks → 1 NVS write / 64 ids */

#define NVS_NS               "evlog"
#define NVS_KEY_RD_SEQ       "rd_seq"
#define NVS_KEY_RD_OFF       "rd_off"
#define NVS_KEY_NID          "nid"

static SemaphoreHandle_t s_mtx = NULL;
static StaticSemaphore_t s_mtx_storage;
static bool s_available = false;

/* Tail (write) file. */
static FILE     *s_wf        = NULL;
static uint32_t  s_tail_seq  = 1;
static long      s_tail_size = 0;

/* Read cursor (the next record to publish). */
static uint32_t  s_rd_seq = 1;
static long      s_rd_off = 0;

/* In-flight slot: the claimed-but-not-yet-acked record starts at the cursor and
 * is s_inflight_len bytes long; mark_synced advances the cursor past it. */
static bool      s_inflight_active = false;
static int64_t   s_inflight_id     = 0;
static long      s_inflight_len     = 0;

/* next_id: hand out from RAM, persist a high-water mark every EVLOG_ID_BLOCK. */
static int64_t   s_next_id  = 1;
static int64_t   s_id_limit = 1;

/* Counters (re-derived on boot). Every present record is unsynced ⇒ total==pending. */
static int64_t   s_pending = 0;

/* Flush / cursor-persist bookkeeping. */
static uint32_t   s_writes_since_flush = 0;
static TickType_t s_last_flush_tick    = 0;
static uint32_t   s_acks_since_persist = 0;

/* A reusable line buffer for claim — only touched under s_mtx. */
static char s_line[EVLOG_LINE_CAP];

/* ── small helpers ───────────────────────────────────────────────────── */

static void evlog_file_path(char *buf, size_t cap, uint32_t seq)
{
    snprintf(buf, cap, "%s/ev-%06u.log", EVLOG_DIR, (unsigned)seq);
}

static long evlog_file_size(uint32_t seq)
{
    char path[64];
    evlog_file_path(path, sizeof path, seq);
    struct stat st;
    return (stat(path, &st) == 0) ? (long)st.st_size : 0;
}

static bool parse_ev_name(const char *name, uint32_t *seq)
{
    if (strncmp(name, "ev-", 3) != 0) return false;
    const char *p = name + 3;
    if (!isdigit((unsigned char)*p)) return false;
    uint32_t v = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10u + (uint32_t)(*p - '0'); p++; }
    if (strcmp(p, ".log") != 0) return false;
    *seq = v;
    return true;
}

/* channel/device/tag/cmd_raw are the only raw fields → strip tab/newline/control
 * so they can't break the line framing; they're short controlled strings anyway. */
static void sanitize_field(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    if (src != NULL) {
        for (size_t i = 0; src[i] != '\0' && j < cap - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[j++] = (c < 0x20 || c == 0x7F) ? '_' : (char)src[i];
        }
    }
    dst[j] = '\0';
}

static void evlog_flush_writer_locked(void)
{
    if (s_wf != NULL) {
        fflush(s_wf);
        fsync(fileno(s_wf));
    }
}

static void evlog_persist_cursor_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_RD_SEQ, s_rd_seq);
    nvs_set_u32(h, NVS_KEY_RD_OFF, (uint32_t)s_rd_off);
    nvs_commit(h);
    nvs_close(h);
}

static void evlog_persist_nid_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, NVS_KEY_NID, (uint64_t)s_id_limit);
    nvs_commit(h);
    nvs_close(h);
}

/* One boot pass over the live records (cursor → tail): the pending count AND the
 * max measure_id present. Reads complete lines only; a partial trailing line in
 * the tail has no '\n' and is correctly excluded. Uses s_line (safe: only init /
 * on_sd_restored call this, never concurrently with a claim). The max id lets us
 * reseed next_id above anything still on the card even when NVS was wiped. */
static void evlog_scan_locked(int64_t *out_pending, int64_t *out_max_id)
{
    int64_t pending = 0, max_id = 0;
    for (uint32_t seq = s_rd_seq; seq <= s_tail_seq; seq++) {
        char path[64];
        evlog_file_path(path, sizeof path, seq);
        FILE *f = fopen(path, "rb");
        if (f == NULL) continue;
        if (seq == s_rd_seq && s_rd_off > 0) {
            if (fseek(f, s_rd_off, SEEK_SET) != 0) { fclose(f); continue; }
        }
        while (fgets(s_line, sizeof s_line, f) != NULL) {
            size_t len = strlen(s_line);
            if (len == 0 || s_line[len - 1] != '\n') break;   /* partial tail */
            int64_t id = (int64_t)strtoll(s_line, NULL, 10);
            if (id > max_id) max_id = id;
            pending++;
        }
        fclose(f);
    }
    if (out_pending) *out_pending = pending;
    if (out_max_id)  *out_max_id  = max_id;
}

/* Parse one (newline-included) record line in place. Returns:
 *   ESP_OK                  — out filled (heap metadata/payload; free with measurement_event_free)
 *   ESP_ERR_INVALID_RESPONSE — malformed; caller skips it
 *   ESP_ERR_NO_MEM          — alloc failed; caller must NOT consume the record */
static esp_err_t parse_record(char *line, size_t len, measurement_event_t *out)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    char *f[9];
    f[0] = line;
    int nf = 1;
    for (char *p = line; *p != '\0' && nf < 9; p++) {
        if (*p == '\t') { *p = '\0'; f[nf++] = p + 1; }
    }
    if (nf < 9) return ESP_ERR_INVALID_RESPONSE;        /* need exactly 9 fields (v2) */

    const char *payload = f[8];
    if (payload[0] != '{' && payload[0] != '[') return ESP_ERR_INVALID_RESPONSE;

    memset(out, 0, sizeof *out);
    out->measure_id     = (int64_t)strtoll(f[0], NULL, 10);
    strncpy(out->channel, f[1], sizeof(out->channel) - 1);
    strncpy(out->device,  f[2], sizeof(out->device)  - 1);
    strncpy(out->tag,     f[3], sizeof(out->tag)     - 1);
    out->start_ticks_ms = (int64_t)strtoll(f[5], NULL, 10);
    out->end_ticks_ms   = (int64_t)strtoll(f[6], NULL, 10);
    out->sync_state     = MEASUREMENT_SYNC_INFLIGHT;

    /* cmd_raw is variable-length (a full multi-segment "arrun …" command) → heap.
     * NULL when the field is empty. */
    if (f[4][0] != '\0') {
        out->cmd_raw = strdup(f[4]);
        if (out->cmd_raw == NULL) return ESP_ERR_NO_MEM;
    }
    if (f[7][0] != '\0') {
        out->metadata_json = strdup(f[7]);
        if (out->metadata_json == NULL) { measurement_event_free(out); return ESP_ERR_NO_MEM; }
    }
    out->payload_json = strdup(payload);
    if (out->payload_json == NULL) {
        measurement_event_free(out);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Discover files, validate the NVS cursor, open the tail for append, recount
 * pending. Caller holds s_mtx (or is single-threaded at init). */
static esp_err_t evlog_open_locked(void)
{
    mkdir(EVLOG_DIR, 0777);

    uint32_t min_seq = 0, max_seq = 0;
    bool any = false;
    DIR *d = opendir(EVLOG_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            uint32_t seq;
            if (parse_ev_name(ent->d_name, &seq)) {
                if (!any) { min_seq = max_seq = seq; any = true; }
                else { if (seq < min_seq) min_seq = seq; if (seq > max_seq) max_seq = seq; }
            }
        }
        closedir(d);
    }
    if (!any) { min_seq = max_seq = 1; }     /* fresh — tail created by fopen("a") below */
    s_tail_seq = max_seq;

    /* Read cursor; default to the oldest file. */
    uint32_t cseq = min_seq, coff32 = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_RD_SEQ, &cseq);
        nvs_get_u32(h, NVS_KEY_RD_OFF, &coff32);
        nvs_close(h);
    }
    /* Clamp to what's actually on the card (drained files were deleted; a torn
     * offset past EOF is pulled back). Over-clamping only re-sends a few events,
     * which at-least-once already tolerates. */
    if (cseq < min_seq) { cseq = min_seq; coff32 = 0; }
    if (cseq > max_seq) { cseq = max_seq; coff32 = 0; }
    long fsz  = evlog_file_size(cseq);
    long coff = (long)coff32;
    if (coff > fsz) coff = fsz;
    s_rd_seq = cseq;
    s_rd_off = coff;

    char path[64];
    evlog_file_path(path, sizeof path, s_tail_seq);
    s_wf = fopen(path, "a");
    if (s_wf == NULL) {
        ESP_LOGE(TAG, "open tail %s failed", path);
        return ESP_FAIL;
    }
    s_tail_size = evlog_file_size(s_tail_seq);

    int64_t max_id = 0;
    evlog_scan_locked(&s_pending, &max_id);

    /* Never hand out an id that collides with a record still on the card. NVS
     * (where next_id's HWM lives) is wiped on every reflash and could be lost to
     * corruption, while the SD log survives — so after a flash the HWM can read
     * back below ids already written. Seed above the log's max to keep measure_ids
     * unique (openJII dedups on them). */
    if (max_id + 1 > s_next_id) {
        s_next_id = max_id + 1;
        if (s_next_id >= s_id_limit) {
            s_id_limit = s_next_id + EVLOG_ID_BLOCK;
            evlog_persist_nid_locked();
        }
    }

    s_writes_since_flush = 0;
    s_last_flush_tick    = xTaskGetTickCount();
    s_acks_since_persist = 0;
    s_inflight_active    = false;

    ESP_LOGI(TAG, "ready: files %u..%u, cursor seq=%u off=%ld, pending=%lld, max_id=%lld, next_id=%lld",
             (unsigned)min_seq, (unsigned)max_seq, (unsigned)s_rd_seq, s_rd_off,
             (long long)s_pending, (long long)max_id, (long long)s_next_id);
    return ESP_OK;
}

/* ── public API ──────────────────────────────────────────────────────── */

esp_err_t event_log_init(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_storage);
    if (s_mtx == NULL) return ESP_ERR_NO_MEM;

    /* next_id resumes at the persisted high-water mark; the first next_id call
     * reserves a fresh block. Gaps across reboots are fine (ids need only be
     * monotonic + unique). */
    uint64_t hwm = 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, NVS_KEY_NID, &hwm);
        nvs_close(h);
    }
    if (hwm < 1) hwm = 1;
    s_next_id  = (int64_t)hwm;
    s_id_limit = (int64_t)hwm;

    if (sdcard_is_mounted()) {
        s_available = (evlog_open_locked() == ESP_OK);
        if (!s_available) ESP_LOGW(TAG, "event log unavailable");
    } else {
        ESP_LOGW(TAG, "SD not mounted — persistence offline");
    }
    return ESP_OK;
}

esp_err_t event_log_on_sd_lost(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_wf != NULL) {
        evlog_flush_writer_locked();
        fclose(s_wf);
        s_wf = NULL;
    }
    evlog_persist_cursor_locked();      /* save progress before the card goes */
    s_available = false;
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "SD lost — event log closed");
    return ESP_OK;
}

esp_err_t event_log_on_sd_restored(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (!s_available) {
        err = evlog_open_locked();
        s_available = (err == ESP_OK);
    }
    xSemaphoreGive(s_mtx);
    if (s_available) ESP_LOGI(TAG, "SD restored — event log reopened");
    return err;
}

esp_err_t event_log_next_id(int64_t *out_id)
{
    if (out_id == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_next_id >= s_id_limit) {
        s_id_limit = s_next_id + EVLOG_ID_BLOCK;
        evlog_persist_nid_locked();
    }
    *out_id = s_next_id++;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_store_event(const measurement_event_desc_t *desc)
{
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available || s_wf == NULL) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int64_t measure_id = desc->measure_id;
    char chan[12], dev[24], tag[16];
    sanitize_field(chan, sizeof chan, desc->channel);
    sanitize_field(dev,  sizeof dev,  desc->device);
    sanitize_field(tag,  sizeof tag,  desc->tag);
    /* cmd_raw is variable-length (a full multi-segment "arrun …" can be ~520 B),
     * so it gets its own heap-sanitized buffer and its own write rather than
     * sharing the fixed header buffer. */
    const char *cmd_src = (desc->cmd_raw != NULL) ? desc->cmd_raw : "";
    size_t cmd_cap = strlen(cmd_src) + 1;
    char *cmd = malloc(cmd_cap);
    if (cmd == NULL) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    sanitize_field(cmd, cmd_cap, cmd_src);
    const char *meta = (desc->metadata_json != NULL && desc->metadata_json[0] != '\0')
                       ? desc->metadata_json : "";
    const char *payload_json = desc->payload_json;

    /* Header split around cmd_raw: "<id>\t<chan>\t<dev>\t<tag>\t" then cmd, then
     * "\t<start>\t<end>\t". */
    char hdr1[96], hdr2[48];
    int h1 = snprintf(hdr1, sizeof hdr1, "%lld\t%s\t%s\t%s\t",
                      (long long)measure_id, chan, dev, tag);
    int h2 = snprintf(hdr2, sizeof hdr2, "\t%lld\t%lld\t",
                      (long long)desc->start_ms, (long long)desc->end_ms);
    if (h1 < 0 || h1 >= (int)sizeof hdr1 || h2 < 0 || h2 >= (int)sizeof hdr2) {
        free(cmd);
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    size_t clen  = strlen(cmd);
    size_t mlen  = strlen(meta);
    size_t plen  = strlen(payload_json);
    size_t total = (size_t)h1 + clen + (size_t)h2 + mlen + 1 /*tab*/ + plen + 1 /*\n*/;
    if (total >= EVLOG_MAX_RECORD) {
        ESP_LOGE(TAG, "record too large (%u B) for id %lld — dropped",
                 (unsigned)total, (long long)measure_id);
        free(cmd);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Roll to a fresh file before the tail would exceed the rotate threshold, so
     * a fully-published file can later be deleted to reclaim space. */
    if (s_tail_size > 0 && s_tail_size + (long)total > EVLOG_ROTATE_BYTES) {
        evlog_flush_writer_locked();
        fclose(s_wf);
        s_tail_seq++;
        char path[64];
        evlog_file_path(path, sizeof path, s_tail_seq);
        s_wf = fopen(path, "a");
        if (s_wf == NULL) {
            ESP_LOGE(TAG, "rotate: open %s failed", path);
            s_available = false;
            free(cmd);
            xSemaphoreGive(s_mtx);
            return ESP_FAIL;
        }
        s_tail_size = 0;
    }

    size_t w = 0;
    w += fwrite(hdr1, 1, (size_t)h1, s_wf);
    w += fwrite(cmd,  1, clen, s_wf);
    w += fwrite(hdr2, 1, (size_t)h2, s_wf);
    w += fwrite(meta, 1, mlen, s_wf);
    w += fwrite("\t", 1, 1, s_wf);
    w += fwrite(payload_json, 1, plen, s_wf);
    w += fwrite("\n", 1, 1, s_wf);
    free(cmd);
    if (w != total) {
        /* A failed/short write (e.g. sdmmc couldn't get a DMA buffer under low
         * heap) leaves a torn partial record. Roll the file back to the last good
         * boundary so the partial can't merge with — and corrupt the framing of —
         * the NEXT record. The event is simply dropped (not counted as pending).
         * Best-effort: under severe OOM even the truncate may fail, but then the
         * reader's skip-bad still discards the torn line on read. */
        ESP_LOGE(TAG, "store_event: short write (%u/%u) for id %lld — SD error; rolling back",
                 (unsigned)w, (unsigned)total, (long long)measure_id);
        fflush(s_wf);
        if (ftruncate(fileno(s_wf), s_tail_size) != 0) {
            ESP_LOGW(TAG, "store_event: rollback truncate failed (torn record left; reader will skip)");
        }
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    s_tail_size += (long)total;
    s_pending++;

    s_writes_since_flush++;
    TickType_t now = xTaskGetTickCount();
    if (s_writes_since_flush >= EVLOG_FLUSH_EVERY_N ||
        (now - s_last_flush_tick) >= pdMS_TO_TICKS(EVLOG_FLUSH_PERIOD_MS)) {
        evlog_flush_writer_locked();
        s_writes_since_flush = 0;
        s_last_flush_tick    = now;
    }

    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_claim_next_event(measurement_event_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (int guard = 0; guard < 100000; guard++) {
        bool is_tail = (s_rd_seq >= s_tail_seq);
        if (is_tail) evlog_flush_writer_locked();   /* push appends to media first */

        char path[64];
        evlog_file_path(path, sizeof path, s_rd_seq);
        FILE *rf = fopen(path, "rb");
        if (rf == NULL) {
            if (!is_tail) {                          /* rotated file already gone → next */
                s_rd_seq++; s_rd_off = 0;
                evlog_persist_cursor_locked();
                continue;
            }
            result = ESP_ERR_NOT_FOUND;              /* tail missing — bail */
            break;
        }
        if (fseek(rf, s_rd_off, SEEK_SET) != 0) { fclose(rf); result = ESP_ERR_NOT_FOUND; break; }

        char *got = fgets(s_line, sizeof s_line, rf);
        if (got == NULL) {                           /* EOF at the cursor */
            fclose(rf);
            if (!is_tail) {                          /* rotated file fully drained → delete */
                remove(path);
                s_rd_seq++; s_rd_off = 0;
                evlog_persist_cursor_locked();
                continue;
            }
            result = ESP_ERR_NOT_FOUND;              /* no new record in the tail yet */
            break;
        }

        size_t len = strlen(got);
        bool complete = (len > 0 && got[len - 1] == '\n');
        if (!complete) {
            if (len == sizeof(s_line) - 1) {         /* over-long/corrupt: no '\n' within cap */
                long skipped = (long)len;
                char *more;
                while ((more = fgets(s_line, sizeof s_line, rf)) != NULL) {
                    size_t l2 = strlen(more);
                    skipped += (long)l2;
                    if (l2 > 0 && more[l2 - 1] == '\n') break;
                }
                fclose(rf);
                ESP_LOGW(TAG, "skipping over-long record seq=%u off=%ld (%ld B)",
                         (unsigned)s_rd_seq, s_rd_off, skipped);
                s_rd_off += skipped;
                if (s_pending > 0) s_pending--;
                evlog_persist_cursor_locked();
                continue;
            }
            fclose(rf);
            if (!is_tail) {                          /* torn tail of a closed file → drop it */
                ESP_LOGW(TAG, "partial record at end of rotated %s — dropping", path);
                remove(path);
                s_rd_seq++; s_rd_off = 0;
                evlog_persist_cursor_locked();
                continue;
            }
            result = ESP_ERR_NOT_FOUND;              /* unflushed partial in tail — try later */
            break;
        }
        fclose(rf);

        esp_err_t pr = parse_record(got, len, out);
        if (pr == ESP_OK) {
            s_inflight_active = true;
            s_inflight_id     = out->measure_id;
            s_inflight_len    = (long)len;
            result = ESP_OK;
            break;
        }
        if (pr == ESP_ERR_NO_MEM) {                  /* don't consume — retry next drain */
            result = ESP_ERR_NO_MEM;
            break;
        }
        /* malformed record → skip past it */
        ESP_LOGW(TAG, "skipping bad record seq=%u off=%ld len=%u",
                 (unsigned)s_rd_seq, s_rd_off, (unsigned)len);
        s_rd_off += (long)len;
        if (s_pending > 0) s_pending--;
        evlog_persist_cursor_locked();
    }

    xSemaphoreGive(s_mtx);
    return result;
}

esp_err_t event_log_mark_event_synced(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_inflight_active && measure_id == s_inflight_id) {
        s_rd_off += s_inflight_len;          /* advance past the published record */
        s_inflight_active = false;
        if (s_pending > 0) s_pending--;
        if (++s_acks_since_persist >= EVLOG_CURSOR_BATCH) {
            evlog_persist_cursor_locked();
            s_acks_since_persist = 0;
        }
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_mark_event_pending(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_inflight_active && measure_id == s_inflight_id) {
        s_inflight_active = false;           /* cursor unchanged → re-read next claim */
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_db_stats(bool *available, int64_t *total,
                             int64_t *pending, int64_t *next_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (available) *available = s_available;
    if (total)     *total     = s_pending;
    if (pending)   *pending   = s_pending;
    if (next_id)   *next_id   = s_next_id;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* ── fn getters ──────────────────────────────────────────────────────── */

measurement_next_id_fn            event_log_get_next_id_fn(void)            { return event_log_next_id; }
measurement_store_event_fn        event_log_get_store_event_fn(void)        { return event_log_store_event; }
measurement_claim_next_event_fn   event_log_get_claim_next_event_fn(void)   { return event_log_claim_next_event; }
measurement_mark_event_synced_fn  event_log_get_mark_event_synced_fn(void)  { return event_log_mark_event_synced; }
measurement_mark_event_pending_fn event_log_get_mark_event_pending_fn(void) { return event_log_mark_event_pending; }
measurement_db_stats_fn           event_log_get_db_stats_fn(void)           { return event_log_db_stats; }

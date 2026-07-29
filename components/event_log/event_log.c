/*
 * event_log.c — append-only event log behind persistence_port.h.
 *
 * Replaces SQLite (which corrupted FATFS/SD via in-place page + header rewrites)
 * with an append-only store-and-forward FIFO. See event_log.h and
 * docs/append-log-persistence-plan.md for the design rationale; the Step-0 spike
 * proved this write pattern is corruption-free on the field card.
 *
 * Concurrency: every public op runs under s_mtx, serialising the Lua task
 * (store), the sync-runner drain (claim plus deferred ack/error marks), the
 * sync-runner watchdog task (cmd_store_status_event/cmd_db_status), and the CLI
 * (stats). MQTT/Wi-Fi event tasks never enter this component: device_commands
 * queues their completions for the drain so FATFS latency cannot stall socket
 * servicing. The RAM claim window is bounded by the same 16-slot/64-KiB
 * contract as the publisher; only its contiguous ACKed prefix is durable.
 */

#include "event_log.h"
#include "sd_card.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
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
#define EVLOG_QUARANTINE     EVLOG_DIR "/quarantine.log"   /* poison events archived here */
#define EVLOG_LINE_CAP       12288            /* max bytes read back per record (incl '\n') */
#define EVLOG_MAX_RECORD     (EVLOG_LINE_CAP - 16)  /* store rejects bigger — must stay readable */
#define EVLOG_ROTATE_BYTES   (256 * 1024)     /* roll the tail file past this size */
#define EVLOG_FLUSH_PERIOD_MS 1500            /* periodic flush backstop (NOT the primary durability lever) */
#define EVLOG_FLUSH_EVERY_N  1                /* fsync each record: at ~0.4 writes/s the write-amp is trivial and
                                               * the brownout-loss window shrinks to one record (audit G3/B2) */
#define EVLOG_CURSOR_BATCH   16               /* persist read cursor every N acks */
#define EVLOG_ID_BLOCK       64               /* reserve next_id in blocks → 1 NVS write / 64 ids */
#define EVLOG_SCAN_MAX_LINES 20000            /* bound the boot pending-count (stat only) */
#define EVLOG_SCAN_MAX_MS    3000             /* …and its wall-clock, so a huge/slow backlog can't churn on */
#define EVLOG_MIN_FREE_BYTES (256 * 1024)     /* storage-full watermark: refuse writes below this (audit C1/C2) */
#define EVLOG_OOM_STUCK_MAX  30               /* NO_MEM retries on one head record → quarantine it (audit D2) */

_Static_assert(PUBLISH_WINDOW_SLOTS > 0 && PUBLISH_WINDOW_SLOTS <= 16,
               "publish window must fit the firmware's 16-slot ACK table");
_Static_assert(PUBLISH_WINDOW_BYTES > 0, "publish window byte budget must be non-zero");

#define NVS_NS               "evlog"
#define NVS_KEY_RD_SEQ       "rd_seq"
#define NVS_KEY_RD_OFF       "rd_off"
#define NVS_KEY_NID          "nid"
#define NVS_KEY_CARDID       "card_id"   /* SDMMC serial bound to the persisted cursor (audit I2/I3) */

static SemaphoreHandle_t s_mtx = NULL;
static StaticSemaphore_t s_mtx_storage;
/* volatile: event_log_on_sd_lost flips this false WITHOUT s_mtx (it may not be
 * able to take the lock in time when a writer is stuck in a failing transfer),
 * so the flag must be observed promptly by the next store. */
static volatile bool s_available = false;
/* Composition-root callback: an SD reopen rebuilds this component's volatile
 * claim window from the durable cursor, so the MQTT peer must discard its RAM
 * correlation table/queue at the same boundary. Stored once during boot. */
static void (*s_reset_notifier)(void) = NULL;
/* Set only under s_mtx when an invariant-recovery path discards the event window.
 * The mutating caller consumes it before releasing s_mtx, then invokes the peer
 * reset outside the lock. This preserves the event_log -> device_commands
 * dependency boundary without leaving the two RAM windows divergent for 60 s. */
static bool s_reset_pending = false;

/* Tail (write) file. */
static FILE     *s_wf        = NULL;
static uint32_t  s_tail_seq  = 1;
static long      s_tail_size = 0;

/* Read cursor (the next record to publish). */
static uint32_t  s_rd_seq = 1;
static long      s_rd_off = 0;

/* RAM-only claim window over the durable read cursor.  Slots are ordered by
 * (seq,off) and remain in this array until they leave the contiguous ACKed
 * prefix.  `claimed=false && !acked` is a reverted record and is always returned
 * before a new offset is admitted; this is what makes a mid-window timeout or
 * disconnect replay FIFO rather than leaking past the gap.  The cursor, rotated
 * file deletion, pending count, and NVS persistence NEVER follow the claim tail.
 * They move only when slot zero is ACKed (or explicitly frontier-quarantined).
 *
 * Per-slot token invariant, paired with device_commands' msg-id table: an
 * unacked claimed record has at most one MQTT latch or one queued completion;
 * event_log itself is mutated only by the sole sync-runner consumer. */
typedef struct {
    int64_t  measure_id;
    uint32_t seq;
    long     off;
    long     len;
    bool     claimed;
    bool     acked;
} evlog_window_slot_t;

static evlog_window_slot_t s_window[PUBLISH_WINDOW_SLOTS];
static size_t              s_window_count = 0;
static size_t              s_window_bytes = 0;

/* next_id: hand out from RAM, persist a high-water mark every EVLOG_ID_BLOCK. */
static int64_t   s_next_id  = 1;
static int64_t   s_id_limit = 1;

/* Counters (re-derived on boot). Every present record is unsynced ⇒ total==pending.
 * s_pending is counted off the boot path by evlog_count_task (see below). */
static int64_t     s_pending    = 0;
static TaskHandle_t s_count_task = NULL;   /* one-shot boot backlog counter */

/* Flush / cursor-persist bookkeeping. */
static uint32_t   s_writes_since_flush = 0;
static TickType_t s_last_flush_tick    = 0;
static uint32_t   s_acks_since_persist = 0;

/* Health / loss accounting (surfaced via event_log_health for the STATUS heartbeat,
 * so silent-skip and drop sites become observable in the field). */
static int64_t   s_skipped       = 0;   /* records/files skipped at the cursor without publishing */
static int64_t   s_dropped       = 0;   /* records refused/dropped at store (too-large, full, short-write) */
static int64_t   s_last_acked_id = 0;   /* highest measure_id confirmed synced (PUBACK) */
/* Storage-full is distinct from card-loss: a full card is HEALTHY, so we pause
 * writes WITHOUT reporting an I/O error (which would unmount it + restart Lua).
 * The drain keeps running, frees space, and store admission re-enables writes. */
static volatile bool s_write_full = false;
/* Head-of-line OOM tracking: a record too big to strdup on the fragmented heap is
 * retried; after EVLOG_OOM_STUCK_MAX strikes on the SAME head it is quarantined so
 * the drain advances instead of reboot-looping. */
static int64_t   s_oom_head_id = 0;
static uint32_t  s_oom_strikes = 0;

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

static void evlog_window_clear_locked(void)
{
    memset(s_window, 0, sizeof s_window);
    s_window_count = 0;
    s_window_bytes = 0;
}

static int evlog_window_find_locked(int64_t measure_id)
{
    for (size_t i = 0; i < s_window_count; i++) {
        if (s_window[i].measure_id == measure_id) return (int)i;
    }
    return -1;
}

static void evlog_window_pop_front_locked(void)
{
    if (s_window_count == 0) return;
    size_t len = (size_t)s_window[0].len;
    if (s_window_count > 1) {
        memmove(&s_window[0], &s_window[1],
                (s_window_count - 1U) * sizeof s_window[0]);
    }
    s_window_count--;
    memset(&s_window[s_window_count], 0, sizeof s_window[0]);
    s_window_bytes = len <= s_window_bytes ? s_window_bytes - len : 0;
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

/* Scan EVLOG_DIR for the lowest/highest ev-NNNNNN.log seq present. Returns true
 * if any log file exists (then min_seq and max_seq are filled). Caller holds
 * s_mtx (or is single-threaded at init). */
static bool evlog_scan_range_locked(uint32_t *min_seq, uint32_t *max_seq)
{
    uint32_t mn = 0, mx = 0;
    bool any = false;
    DIR *d = opendir(EVLOG_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            uint32_t seq;
            if (parse_ev_name(ent->d_name, &seq)) {
                if (!any) { mn = mx = seq; any = true; }
                else { if (seq < mn) mn = seq; if (seq > mx) mx = seq; }
            }
        }
        closedir(d);
    }
    if (any) { if (min_seq) *min_seq = mn; if (max_seq) *max_seq = mx; }
    return any;
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

/* Flush + fsync the tail. Returns ESP_FAIL if the underlying media write failed
 * (a full/dying card), so callers can surface it instead of counting torn data as
 * durable. Returns ESP_OK when there is nothing open to flush. */
static esp_err_t evlog_flush_writer_locked(void)
{
    if (s_wf == NULL) return ESP_OK;
    if (fflush(s_wf) != 0 || fsync(fileno(s_wf)) != 0 || ferror(s_wf)) {
        clearerr(s_wf);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Reopen just the tail file for append after a prior open/rotate failure closed it,
 * without the full re-scan of evlog_open_locked. Caller holds s_mtx. */
static esp_err_t evlog_reopen_tail_locked(void)
{
    if (s_wf != NULL) return ESP_OK;
    char path[64];
    evlog_file_path(path, sizeof path, s_tail_seq);
    s_wf = fopen(path, "a");
    if (s_wf == NULL) return ESP_FAIL;
    s_tail_size = evlog_file_size(s_tail_seq);
    return ESP_OK;
}

/* Copy rec_len raw bytes at the current cursor to quarantine.log (archive-before-skip).
 * Used only by the OOM head-of-line escape, where parse_record has already mangled
 * s_line, so we re-read the raw record from disk. Clobbers s_line. Caller holds s_mtx.
 *
 * Window-safety contract: expected_measure_id MUST still identify the record at
 * the persisted cursor frontier. The claim window can parse a later slot, but
 * this head-only escape may never archive/advance that later slot's length at
 * s_rd_off. Re-reading and comparing the frontier id makes mismatch fail closed. */
static bool evlog_archive_head_locked(long rec_len, int64_t expected_measure_id)
{
    if (rec_len <= 0 || rec_len >= (long)sizeof s_line) return false;
    char path[64];
    evlog_file_path(path, sizeof path, s_rd_seq);
    FILE *rf = fopen(path, "rb");
    if (rf == NULL) return false;
    bool ok = false;
    if (fseek(rf, s_rd_off, SEEK_SET) == 0 &&
        fread(s_line, 1, (size_t)rec_len, rf) == (size_t)rec_len) {
        s_line[rec_len] = '\0';
        int64_t frontier_id = (int64_t)strtoll(s_line, NULL, 10);
        if (s_line[rec_len - 1] != '\n' || frontier_id != expected_measure_id) {
            ESP_LOGW(TAG, "OOM archive refused: cursor seq=%u off=%ld holds id=%lld, expected id=%lld",
                     (unsigned)s_rd_seq, s_rd_off, (long long)frontier_id,
                     (long long)expected_measure_id);
        } else {
            FILE *qf = fopen(EVLOG_QUARANTINE, "a");
            if (qf != NULL) {
                size_t w = fwrite(s_line, 1, (size_t)rec_len, qf);
                fflush(qf);
                fsync(fileno(qf));
                fclose(qf);
                ok = (w == (size_t)rec_len);
            }
        }
    }
    fclose(rf);
    return ok;
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

/* Delete/step over a rotated file only when the durable cursor itself has
 * reached its EOF.  The claim tail may already be in a later file, but it is
 * deliberately irrelevant here: a reboot must still find every un-ACKed byte
 * at or after the cursor.  File-boundary persistence matches the old frequency
 * (once per drained file) and is safe even between 16-ACK cursor batches. */
static void evlog_normalize_cursor_locked(void)
{
    while (s_rd_seq < s_tail_seq) {
        char path[64];
        evlog_file_path(path, sizeof path, s_rd_seq);
        struct stat st;
        /* A transient stat failure is not proof of EOF.  Leave the cursor in
         * place so the normal claim path can distinguish a genuinely missing
         * file from an I/O failure; treating the old helper's synthetic zero as
         * EOF here could silently skip a whole rotated file. */
        if (stat(path, &st) != 0 || s_rd_off < (long)st.st_size) break;

        remove(path);
        s_rd_seq++;
        s_rd_off = 0;
        evlog_persist_cursor_locked();
    }
}

/* Advance only the contiguous ACKed prefix.  Out-of-order ACKs merely flip a
 * slot flag; neither the RAM cursor nor NVS can cross the first unacked slot.
 * The batched persistence rule is unchanged: an unpersisted RAM advance can
 * replay duplicates after reboot, but the stored cursor can never skip data. */
static void evlog_advance_acked_prefix_locked(void)
{
    while (s_window_count > 0 && s_window[0].acked) {
        evlog_window_slot_t front = s_window[0];
        if (front.seq != s_rd_seq || front.off != s_rd_off) {
            ESP_LOGE(TAG, "window frontier mismatch: cursor=%u:%ld slot=%u:%ld id=%lld — RESETTING volatile window; durable cursor will replay",
                     (unsigned)s_rd_seq, s_rd_off, (unsigned)front.seq, front.off,
                     (long long)front.measure_id);
            /* The cursor is durable truth. Holding this impossible RAM shape
             * forever turns one invariant violation into a permanent publisher
             * outage; abandoning claims costs only bounded duplicates. Stale
             * MQTT completions/latches must be discarded at the same recovery
             * boundary; the caller fires the registered reset after s_mtx is
             * released, then this cursor is claimed again. */
            evlog_window_clear_locked();
            s_reset_pending = true;
            return;
        }

        s_rd_seq = front.seq;
        s_rd_off = front.off + front.len;
        s_last_acked_id = front.measure_id;
        if (s_pending > 0) s_pending--;
        evlog_window_pop_front_locked();

        if (++s_acks_since_persist >= EVLOG_CURSOR_BATCH) {
            evlog_persist_cursor_locked();
            s_acks_since_persist = 0;
        }
        evlog_normalize_cursor_locked();
    }
}

static void evlog_persist_nid_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, NVS_KEY_NID, (uint64_t)s_id_limit);
    nvs_commit(h);
    nvs_close(h);
}

/* The highest measure_id on the card is the newest record. Records are appended in
 * strictly-ascending id order, so it lives in the last non-empty file (the tail, or
 * the file just below it if the tail was freshly rotated and is still empty). Scan
 * only that one (≤EVLOG_ROTATE_BYTES) file, so next_id can be reseeded on boot
 * WITHOUT walking the whole backlog. Uses s_line — caller holds s_mtx / is init. */
static int64_t evlog_tail_max_id_locked(void)
{
    uint32_t seq = s_tail_seq;
    while (seq >= 1) {
        char path[64];
        evlog_file_path(path, sizeof path, seq);
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            int64_t max_id = 0;
            while (fgets(s_line, sizeof s_line, f) != NULL) {
                size_t len = strlen(s_line);
                if (len == 0 || s_line[len - 1] != '\n') break;   /* partial tail */
                int64_t id = (int64_t)strtoll(s_line, NULL, 10);
                if (id > max_id) max_id = id;
            }
            fclose(f);
            if (max_id > 0) return max_id;
        }
        if (seq == 1) break;
        seq--;
    }
    return 0;
}

/* Full-directory scan for the highest measure_id across ALL ev files. Used only on
 * a detected card swap, where the tail-only scan could miss a higher id in a lower-seq
 * file and hand out a colliding measure_id on the foreign card (audit I3). Caller
 * holds s_mtx; uses s_line. */
static int64_t evlog_scan_max_id_locked(void)
{
    int64_t max_id = 0;
    uint32_t mn = 0, mx = 0;
    if (!evlog_scan_range_locked(&mn, &mx)) return 0;
    for (uint32_t seq = mn; seq <= mx; seq++) {
        char path[64];
        evlog_file_path(path, sizeof path, seq);
        FILE *f = fopen(path, "rb");
        if (f == NULL) continue;
        while (fgets(s_line, sizeof s_line, f) != NULL) {
            size_t len = strlen(s_line);
            if (len == 0 || s_line[len - 1] != '\n') break;   /* partial tail */
            int64_t id = (int64_t)strtoll(s_line, NULL, 10);
            if (id > max_id) max_id = id;
        }
        fclose(f);
    }
    return max_id;
}

static uint32_t evlog_nvs_get_cardid(void)
{
    uint32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_CARDID, &v);
        nvs_close(h);
    }
    return v;
}

static void evlog_nvs_set_cardid(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_CARDID, v);
    nvs_commit(h);
    nvs_close(h);
}

/* One-shot task: count the pending backlog (cursor → tail) OFF the boot path, so a
 * large offline accumulation (Wi-Fi down → nothing drains) can't stall app_main
 * before the console starts. BOUNDED by line count and wall-clock — beyond that the
 * count is a floor (true value ≥ reported), which is fine: it is a stat only, and
 * claim/publish walk the cursor regardless. Runs under s_mtx (serialised with
 * store/claim/drain — no concurrent FATFS handles) but only briefly (≤EVLOG_SCAN_MAX_MS).
 * next_id was already seeded synchronously in evlog_open_locked, so a partial count
 * never risks id collisions. */
/* Count PENDING records from the cursor (s_rd_seq/off) to the tail, BOUNDED by
 * line count and wall-clock so a huge backlog can't churn on. *capped_out (may be
 * NULL) is set true if a bound was hit — then the return is a floor (true ≥ it).
 * Caller holds s_mtx; uses the shared s_line buffer. */
static int64_t evlog_scan_pending_locked(bool *capped_out)
{
    int64_t pending = 0;
    bool    capped  = false;
    const TickType_t t0 = xTaskGetTickCount();
    for (uint32_t seq = s_rd_seq; seq <= s_tail_seq && !capped; seq++) {
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
            pending++;
            if (pending >= EVLOG_SCAN_MAX_LINES ||
                (xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(EVLOG_SCAN_MAX_MS)) {
                capped = true;
                break;
            }
        }
        fclose(f);
    }
    if (capped_out) *capped_out = capped;
    return pending;
}

static void evlog_count_task(void *arg)
{
    (void)arg;
    bool capped = false;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pending    = evlog_scan_pending_locked(&capped);
    int64_t pending = s_pending;
    s_count_task = NULL;
    xSemaphoreGive(s_mtx);

    if (capped) {
        ESP_LOGW(TAG, "pending backlog >= %lld (count capped) — drain via Wi-Fi/MQTT "
                      "or clear /sdcard/events", (long long)pending);
    } else {
        ESP_LOGI(TAG, "pending backlog counted: %lld", (long long)pending);
    }
    vTaskDelete(NULL);
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
    /* A prior event_log_on_sd_lost() that timed out on s_mtx may have left the
     * tail FILE* open (card gone, couldn't close). Close it now before we reopen
     * so the descriptor can't leak across a loss→restore cycle. */
    if (s_wf != NULL) { fclose(s_wf); s_wf = NULL; }

    mkdir(EVLOG_DIR, 0777);

    uint32_t min_seq = 0, max_seq = 0;
    if (!evlog_scan_range_locked(&min_seq, &max_seq)) {
        min_seq = max_seq = 1;               /* fresh — tail created by fopen("a") below */
    }
    s_tail_seq = max_seq;

    /* Card-identity guard: the read cursor + next_id HWM live in NVS (internal flash),
     * but the log lives on the SD. If the card was swapped/restored while NVS persisted,
     * applying the stale cursor to a DIFFERENT card's log would strand its lower-seq
     * files (the ~35k-record gap failure) or hand out colliding measure_ids. Detect the
     * swap by CID serial and, on mismatch, re-publish this card's log from the oldest
     * file and reseed next_id from a FULL scan (audit I2/I3). */
    uint32_t card_serial = sdcard_card_serial();
    uint32_t prev_serial = evlog_nvs_get_cardid();
    bool card_swapped = (prev_serial != 0 && card_serial != 0 && prev_serial != card_serial);
    if (card_swapped) {
        ESP_LOGW(TAG, "SD card changed (serial 0x%08x -> 0x%08x) — re-publishing this card's "
                      "log from oldest, reseeding id from full scan",
                 (unsigned)prev_serial, (unsigned)card_serial);
    }

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
    if (card_swapped) { cseq = min_seq; coff = 0; }   /* foreign cursor → start at oldest */
    s_rd_seq = cseq;
    s_rd_off = coff;

    char path[64];
    evlog_file_path(path, sizeof path, s_tail_seq);

    /* Repair a brownout-torn final record: an ungraceful reset (brownout/POR/panic
     * bypasses the shutdown-handler fsync) can leave the tail ending mid-record with
     * no '\n'. Truncate back to the last newline so the next append can't concatenate
     * into — and corrupt the framing of — the torn record (audit B3). Only truncate
     * when a newline is found in the trailing window; if none is (an over-long/corrupt
     * fragment), leave it for the reader's over-long-skip to handle rather than nuke
     * good data. */
    long tsz = evlog_file_size(s_tail_seq);
    if (tsz > 0) {
        FILE *tf = fopen(path, "rb+");
        if (tf != NULL) {
            long scan = (tsz < EVLOG_LINE_CAP) ? tsz : EVLOG_LINE_CAP;
            if (fseek(tf, tsz - scan, SEEK_SET) == 0) {
                size_t got = fread(s_line, 1, (size_t)scan, tf);
                long last_nl = -1;
                for (long i = (long)got - 1; i >= 0; i--) {
                    if (s_line[i] == '\n') { last_nl = i; break; }
                }
                if (last_nl >= 0) {
                    long good_end = tsz - scan + last_nl + 1;
                    if (good_end < tsz && ftruncate(fileno(tf), good_end) == 0) {
                        ESP_LOGW(TAG, "repaired torn tail %s: %ld -> %ld B", path, tsz, good_end);
                    }
                }
            }
            fclose(tf);
        }
    }

    s_wf = fopen(path, "a");
    if (s_wf == NULL) {
        ESP_LOGE(TAG, "open tail %s failed", path);
        return ESP_FAIL;
    }
    s_tail_size = evlog_file_size(s_tail_seq);

    /* Seed next_id from the card synchronously — cheap: only the tail file (records are
     * appended in ascending id order, so the tail holds the max). On a card SWAP the
     * tail-only shortcut is unsafe (a lower-seq file could hold a higher id from the
     * foreign device), so scan the whole directory once. */
    int64_t max_id = card_swapped ? evlog_scan_max_id_locked() : evlog_tail_max_id_locked();
    s_pending = 0;   /* provisional; evlog_count_task fills it in shortly */

    /* Bind this card's serial to the cursor for next-boot swap detection. */
    if (card_serial != 0 && card_serial != prev_serial) {
        evlog_nvs_set_cardid(card_serial);
    }

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
    evlog_window_clear_locked();

    ESP_LOGI(TAG, "ready: files %u..%u, cursor seq=%u off=%ld, max_id=%lld, next_id=%lld (pending counting in background)",
             (unsigned)min_seq, (unsigned)max_seq, (unsigned)s_rd_seq, s_rd_off,
             (long long)max_id, (long long)s_next_id);

    /* Count the backlog off the boot path (see evlog_count_task). Guard against a
     * duplicate if a prior count is still running (e.g. rapid SD loss/restore). */
    if (s_count_task == NULL) {
        if (xTaskCreate(evlog_count_task, "evlog_count", 4096, NULL, 3, &s_count_task) != pdPASS) {
            s_count_task = NULL;   /* count skipped; pending stays provisional 0 (stat only) */
        }
    }
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

void event_log_set_reset_notifier(void (*fn)(void))
{
    s_reset_notifier = fn;
}

esp_err_t event_log_on_sd_lost(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;

    /* Stop new stores FIRST, without waiting for the lock. A writer stuck in a
     * failing multi-second fwrite still holds s_mtx, but the next store to acquire
     * it will see s_available=false (and sdcard_io_lost()) and bail. This is what
     * makes the close starvation-proof: even if we can't grab the lock to close
     * the file, the writers are already shut off. */
    s_available = false;

    /* Generous timeout: must outlast one in-flight store's worst-case failing
     * write burst on a gone card (writes time out at ~5 s each in the driver), so
     * the close actually happens instead of leaking the open tail. If it still
     * times out, the FILE* is closed defensively on the next evlog_open_locked. */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "SD lost — stores halted, but tail close deferred (lock busy)");
        return ESP_ERR_TIMEOUT;
    }
    if (s_wf != NULL) {
        /* If the card is already latched lost, the FATFS volume may be freed by the
         * monitor's teardown at any moment — flushing/closing the handle would race
         * that free (UAF, audit R-8). Just abandon the handle; the next
         * evlog_open_locked closes it defensively (a bounded, one-per-loss leak). */
        if (!sdcard_io_lost()) {
            evlog_flush_writer_locked();    /* best-effort; card believed present */
            fclose(s_wf);
        }
        s_wf = NULL;
    }
    evlog_persist_cursor_locked();      /* save progress before the card goes */
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "SD lost — event log closed");
    return ESP_OK;
}

esp_err_t event_log_prepare_shutdown(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;

    /* Pre-reboot drain: flush + fsync the periodically-buffered tail writes (store
     * flushes only every EVLOG_FLUSH_EVERY_N records / EVLOG_FLUSH_PERIOD_MS, so up
     * to a few records sit unflushed), persist the read cursor, and CLOSE the tail
     * so FATFS can finalize its directory entry cleanly when sdcard_unmount() runs
     * next. Every esp_restart() otherwise fired with no fsync/unmount, risking a
     * torn FAT/dir-entry metadata write. Bounded lock wait — a reboot must not hang
     * on a stuck writer; if it times out the data already fsync'd at the last
     * periodic flush is still safe, we just skip the final few records. */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "pre-reboot flush skipped (lock busy) — last periodic flush stands");
        return ESP_ERR_TIMEOUT;
    }
    if (s_wf != NULL) {
        evlog_flush_writer_locked();
        fclose(s_wf);
        s_wf = NULL;
    }
    evlog_persist_cursor_locked();
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t event_log_on_sd_restored(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    bool reopened = false;
    if (!s_available) {
        err = evlog_open_locked();
        reopened = (err == ESP_OK);
    }
    bool available = s_available;
    xSemaphoreGive(s_mtx);
    /* Keep admission closed across the whole two-component reset. A claimant that
     * runs after reopen but before peer reconciliation observes NOT_SUPPORTED,
     * never a fresh event window paired with stale MQTT latches/completions.
     * The notifier must run outside s_mtx; publish availability only after it has
     * returned, using a brief re-take so every normal claimant sees one atomic
     * false -> reconciled -> true transition. */
    if (reopened) {
        if (s_reset_notifier != NULL) s_reset_notifier();
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "SD reopened but availability publish timed out — holding event log offline");
            return ESP_ERR_TIMEOUT;
        }
        s_available = true;
        available = true;
        xSemaphoreGive(s_mtx);
    }
    if (available) ESP_LOGI(TAG, "SD restored — event log reopened");
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

static esp_err_t event_log_store_impl(const measurement_event_desc_t *desc)
{
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    /* Lock-free early-out: if the card has been latched lost, don't even take the
     * lock or touch the (gone) media — bail before re-arming a failing write. */
    if (sdcard_io_lost()) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available || sdcard_io_lost()) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Self-heal: a prior rotate/open failure may have closed the tail without
     * latching the log offline (see rotate path). Reopen it here rather than
     * staying dark for the rest of the session (audit D1). */
    if (s_wf == NULL && evlog_reopen_tail_locked() != ESP_OK) {
        sdcard_report_io_error();
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Storage-full admission control: a full card is healthy, so refuse cleanly
     * (no I/O-error report → no unmount/Lua-restart thrash) and let the drain free
     * space. Re-enable once the drain has recovered headroom (audit C1/C2). */
    if (s_write_full) {
        uint64_t freeb = 0;
        if (sdcard_free_bytes(&freeb) == ESP_OK && freeb >= EVLOG_MIN_FREE_BYTES) {
            s_write_full = false;
            ESP_LOGI(TAG, "SD storage recovered (%llu B free) — writes resumed",
                     (unsigned long long)freeb);
        } else {
            s_dropped++;
            xSemaphoreGive(s_mtx);
            return ESP_ERR_NO_MEM;
        }
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
        s_dropped++;
        free(cmd);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Roll to a fresh file before the tail would exceed the rotate threshold, so
     * a fully-published file can later be deleted to reclaim space. */
    if (s_tail_size > 0 && s_tail_size + (long)total > EVLOG_ROTATE_BYTES) {
        if (evlog_flush_writer_locked() != ESP_OK) sdcard_report_io_error();
        fclose(s_wf);
        s_wf = NULL;
        s_tail_seq++;
        char path[64];
        evlog_file_path(path, sizeof path, s_tail_seq);
        s_wf = fopen(path, "a");
        if (s_wf == NULL) {
            /* Do NOT latch the log offline: a transient open failure (heap/DMA OOM)
             * must not disable persistence for the rest of the session. Drop this one
             * record; the next store's self-heal reopens the (now-incremented) tail
             * (audit D1). */
            ESP_LOGE(TAG, "rotate: open %s failed — retrying next store", path);
            s_dropped++;
            free(cmd);
            xSemaphoreGive(s_mtx);
            sdcard_report_io_error();
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
        ESP_LOGE(TAG, "store_event: short write (%u/%u) for id %lld — rolling back",
                 (unsigned)w, (unsigned)total, (long long)measure_id);
        fflush(s_wf);
        if (ftruncate(fileno(s_wf), s_tail_size) != 0) {
            ESP_LOGW(TAG, "store_event: rollback truncate failed (torn record left; reader will skip)");
        }
        s_dropped++;
        /* Distinguish a FULL card (healthy — pause writes, keep draining) from a
         * pulled/dead one (report I/O error → latch → unmount). A false unmount of a
         * full-but-healthy card causes the remount/Lua-restart thrash (audit C1). */
        uint64_t freeb = 0;
        bool full = (errno == ENOSPC) ||
                    (sdcard_free_bytes(&freeb) == ESP_OK && freeb < EVLOG_MIN_FREE_BYTES);
        if (full) {
            if (!s_write_full) {
                ESP_LOGW(TAG, "SD storage full (%llu B free) — writes paused, draining to reclaim",
                         (unsigned long long)freeb);
            }
            s_write_full = true;
            xSemaphoreGive(s_mtx);
            return ESP_ERR_NO_MEM;      /* full is not card-loss: no report_io_error */
        }
        xSemaphoreGive(s_mtx);
        sdcard_report_io_error();       /* a pulled/dead card latches loss after a few of these */
        return ESP_FAIL;
    }
    sdcard_report_io_ok();          /* good write → reset the consecutive-failure streak */
    s_tail_size += (long)total;
    s_pending++;

    s_writes_since_flush++;
    TickType_t now = xTaskGetTickCount();
    if (s_writes_since_flush >= EVLOG_FLUSH_EVERY_N ||
        (now - s_last_flush_tick) >= pdMS_TO_TICKS(EVLOG_FLUSH_PERIOD_MS)) {
        if (evlog_flush_writer_locked() != ESP_OK) {
            /* The record's bytes reached stdio but fsync failed — treat as an I/O
             * error so a dying card is caught rather than counting torn data durable. */
            xSemaphoreGive(s_mtx);
            sdcard_report_io_error();
            return ESP_FAIL;
        }
        s_writes_since_flush = 0;
        s_last_flush_tick    = now;
    }

    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

static esp_err_t event_log_claim_impl(measurement_event_t *out)
{
    memset(out, 0, sizeof *out);
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Claim-order contract:
     *   1. the oldest reverted, unacked slot already in the window;
     *   2. otherwise the record immediately after the claim tail;
     *   3. never a new offset while any reverted slot exists.
     * ACKed-but-not-prefix slots remain occupied but are never re-published.
     * This ordering is what repairs a hole before extending the window. */
    for (size_t i = 0; i < s_window_count; i++) {
        evlog_window_slot_t *slot = &s_window[i];
        if (slot->acked || slot->claimed) continue;

        bool is_tail = slot->seq >= s_tail_seq;
        if (is_tail && evlog_flush_writer_locked() != ESP_OK) {  /* push appends to media first */
            sdcard_report_io_error();
            xSemaphoreGive(s_mtx);
            return ESP_FAIL;
        }

        char path[64];
        evlog_file_path(path, sizeof path, slot->seq);
        FILE *rf = fopen(path, "rb");
        if (rf == NULL || fseek(rf, slot->off, SEEK_SET) != 0 ||
            fgets(s_line, sizeof s_line, rf) == NULL) {
            if (rf != NULL) fclose(rf);
            ESP_LOGE(TAG, "re-claim failed for id=%lld at %u:%ld — holding window",
                     (long long)slot->measure_id, (unsigned)slot->seq, slot->off);
            xSemaphoreGive(s_mtx);
            return ESP_FAIL;
        }
        fclose(rf);

        size_t len = strlen(s_line);
        int64_t disk_id = (int64_t)strtoll(s_line, NULL, 10);
        if ((long)len != slot->len || len == 0 || s_line[len - 1] != '\n' ||
            disk_id != slot->measure_id) {
            ESP_LOGE(TAG, "re-claim identity mismatch at %u:%ld: id=%lld len=%u, expected id=%lld len=%ld",
                     (unsigned)slot->seq, slot->off, (long long)disk_id,
                     (unsigned)len, (long long)slot->measure_id, slot->len);
            xSemaphoreGive(s_mtx);
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t parsed = parse_record(s_line, len, out);
        if (parsed != ESP_OK || out->measure_id != slot->measure_id) {
            measurement_event_free(out);
            xSemaphoreGive(s_mtx);
            return parsed == ESP_OK ? ESP_ERR_INVALID_STATE : parsed;
        }
        slot->claimed = true;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }

    if (s_window_count >= PUBLISH_WINDOW_SLOTS) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* The scan position follows the claim tail, never the cursor unless the
     * window is empty.  EOF traversal may move this local position across files,
     * but only cursor normalization below is permitted to delete one. */
    evlog_normalize_cursor_locked();
    uint32_t scan_seq = s_rd_seq;
    long scan_off = s_rd_off;
    if (s_window_count > 0) {
        const evlog_window_slot_t *last = &s_window[s_window_count - 1U];
        scan_seq = last->seq;
        scan_off = last->off + last->len;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (int guard = 0; guard < 100000; guard++) {
        bool at_cursor = s_window_count == 0 && scan_seq == s_rd_seq && scan_off == s_rd_off;
        bool is_tail = (scan_seq >= s_tail_seq);
        if (is_tail && evlog_flush_writer_locked() != ESP_OK) {
            sdcard_report_io_error();
            result = ESP_FAIL;
            break;
        }

        char path[64];
        evlog_file_path(path, sizeof path, scan_seq);
        errno = 0;
        FILE *rf = fopen(path, "rb");
        if (rf == NULL) {
            if (!is_tail && errno == ENOENT) {
                if (at_cursor) {
                    ESP_LOGW(TAG, "ev-%06u.log missing at cursor — skipping", (unsigned)scan_seq);
                    s_skipped++;
                    s_rd_seq++; s_rd_off = 0;
                    evlog_persist_cursor_locked();
                    scan_seq = s_rd_seq;
                    scan_off = s_rd_off;
                    continue;
                }
                /* Do not create a later-file slot across a missing-file gap.
                 * Once the existing window closes, that gap becomes the cursor
                 * and the audited frontier-only skip above owns the decision. */
                result = ESP_ERR_INVALID_STATE;
                break;
            }
            if (!is_tail) sdcard_report_io_error();
            result = is_tail ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            break;
        }
        if (fseek(rf, scan_off, SEEK_SET) != 0) {
            fclose(rf);
            result = ESP_ERR_NOT_FOUND;
            break;
        }

        char *got = fgets(s_line, sizeof s_line, rf);
        if (got == NULL) {
            if (ferror(rf)) {
                clearerr(rf); fclose(rf);
                sdcard_report_io_error();
                result = ESP_FAIL;
                break;
            }
            fclose(rf);
            if (!is_tail) {
                if (at_cursor) {
                    remove(path);
                    s_rd_seq++; s_rd_off = 0;
                    evlog_persist_cursor_locked();
                }
                scan_seq++;
                scan_off = 0;
                continue;
            }
            result = ESP_ERR_NOT_FOUND;
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
                if (ferror(rf)) {                    /* over-long scan hit a read error → don't skip */
                    clearerr(rf); fclose(rf);
                    sdcard_report_io_error();
                    result = ESP_FAIL;
                    break;
                }
                fclose(rf);
                if (!at_cursor) {
                    /* A later corrupt record cannot be skipped around the
                     * unacked frontier.  Let the prefix close, then the normal
                     * cursor-owned skip path handles it. */
                    result = ESP_ERR_INVALID_STATE;
                    break;
                }
                ESP_LOGW(TAG, "skipping over-long record seq=%u off=%ld (%ld B)",
                         (unsigned)scan_seq, scan_off, skipped);
                s_rd_off += skipped;
                scan_off = s_rd_off;
                s_skipped++;
                if (s_pending > 0) s_pending--;
                evlog_persist_cursor_locked();
                continue;
            }
            if (ferror(rf)) {                        /* partial line due to a read error, not a torn record */
                clearerr(rf); fclose(rf);
                sdcard_report_io_error();
                result = ESP_FAIL;
                break;
            }
            fclose(rf);
            if (!is_tail && at_cursor) {             /* torn tail of a closed file → drop it */
                ESP_LOGW(TAG, "partial record at end of rotated %s — dropping", path);
                remove(path);
                s_rd_seq++; s_rd_off = 0;
                scan_seq = s_rd_seq; scan_off = 0;
                s_skipped++;
                evlog_persist_cursor_locked();
                continue;
            }
            result = is_tail ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
            break;
        }
        fclose(rf);

        /* Raw-record bytes are a conservative, persistence-owned first gate.
         * device_commands applies the exact envelope-byte gate before publish;
         * both must fit, so JSON framing can never push the MQTT window over
         * PUBLISH_WINDOW_BYTES.  A reverted slot bypasses this check because its
         * bytes are already charged above. */
        if (s_window_bytes + len > PUBLISH_WINDOW_BYTES) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        int64_t record_id = (int64_t)strtoll(got, NULL, 10);
        esp_err_t pr = parse_record(got, len, out);
        if (pr == ESP_OK) {
            evlog_window_slot_t *slot = &s_window[s_window_count++];
            *slot = (evlog_window_slot_t) {
                .measure_id = out->measure_id,
                .seq        = scan_seq,
                .off        = scan_off,
                .len        = (long)len,
                .claimed    = true,
                .acked      = false,
            };
            s_window_bytes += len;
            s_oom_head_id     = 0;   /* made progress — clear OOM strike tracking */
            s_oom_strikes     = 0;
            result = ESP_OK;
            break;
        }
        if (pr == ESP_ERR_NO_MEM) {
            /* Fragmented heap can't strdup this (oversized) record. Retrying it
             * forever head-of-line-blocks the whole backlog into an hourly reboot
             * loop (audit D2). After EVLOG_OOM_STUCK_MAX strikes on the SAME head,
             * quarantine it by raw line so the drain advances. parse_record has
             * mangled s_line, so archive re-reads the raw record from disk. */
            if (!at_cursor) {
                /* A later-slot allocation failure may not feed the destructive
                 * frontier OOM escape at all. Wait for the current prefix to
                 * close, then retry/count it when it is exactly the cursor. */
                result = ESP_ERR_NO_MEM;
                break;
            }
            if (record_id == s_oom_head_id) s_oom_strikes++;
            else { s_oom_head_id = record_id; s_oom_strikes = 1; }
            if (s_oom_strikes >= EVLOG_OOM_STUCK_MAX &&
                evlog_archive_head_locked((long)len, record_id)) {
                ESP_LOGW(TAG, "OOM-stuck record id=%lld quarantined after %u strikes — drain unblocked",
                         (long long)record_id, (unsigned)s_oom_strikes);
                s_rd_off += (long)len;
                scan_off = s_rd_off;
                if (s_pending > 0) s_pending--;
                s_skipped++;
                s_oom_head_id = 0; s_oom_strikes = 0;
                evlog_persist_cursor_locked();
                continue;
            }
            result = ESP_ERR_NO_MEM;                 /* don't consume — retry next drain */
            break;
        }
        /* Malformed records may be skipped only when they are at the cursor
         * frontier.  With claim!=cursor, stop and let earlier slots close first;
         * advancing s_rd_off here would be the ticket-04 m5 data-loss bug. */
        if (!at_cursor) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        ESP_LOGW(TAG, "skipping bad record seq=%u off=%ld len=%u",
                 (unsigned)s_rd_seq, s_rd_off, (unsigned)len);
        s_rd_off += (long)len;
        scan_off = s_rd_off;
        s_skipped++;
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
    int idx = evlog_window_find_locked(measure_id);
    if (idx < 0 || !s_window[idx].claimed || s_window[idx].acked) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* Clearing `claimed` records that the MQTT latch has already been detached
     * into the completion queue.  Later ACKed slots remain in-place; only the
     * helper's slot-zero loop is allowed to advance the cursor. */
    s_window[idx].claimed = false;
    s_window[idx].acked = true;
    evlog_advance_acked_prefix_locked();
    bool reset_pending = s_reset_pending;
    s_reset_pending = false;
    xSemaphoreGive(s_mtx);
    /* Invariant recovery cleared event_log's volatile window. Reset the peer only
     * after releasing s_mtx so its bounded portMUX/queue work cannot invert locks. */
    if (reset_pending && s_reset_notifier != NULL) s_reset_notifier();
    return ESP_OK;
}

esp_err_t event_log_mark_event_pending(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int idx = evlog_window_find_locked(measure_id);
    if (idx < 0 || !s_window[idx].claimed || s_window[idx].acked) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* Revert exactly one slot.  Its seq/off/len charge remains in the window,
     * and claim_next's FIFO scan returns it before admitting any newer offset. */
    s_window[idx].claimed = false;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

static esp_err_t event_log_quarantine_impl(int64_t measure_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) { xSemaphoreGive(s_mtx); return ESP_ERR_NOT_SUPPORTED; }

    /* Window-safety contract: quarantine is deliberately a frontier-only
     * operation. Re-read s_rd_seq/s_rd_off under s_mtx and verify measure_id
     * before any archive or cursor mutation. When ticket 05 lets claim position
     * move ahead of the cursor, a stuck non-frontier slot MUST receive
     * ESP_ERR_INVALID_STATE rather than archive an innocent frontier record. */
    if (s_window_count > 0 &&
        (s_window[0].measure_id != measure_id || s_window[0].seq != s_rd_seq ||
         s_window[0].off != s_rd_off || s_window[0].claimed || s_window[0].acked)) {
        ESP_LOGW(TAG, "quarantine refused for non-pending frontier id=%lld; frontier id=%lld at %u:%ld",
                 (long long)measure_id, (long long)s_window[0].measure_id,
                 (unsigned)s_window[0].seq, s_window[0].off);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_rd_seq >= s_tail_seq) evlog_flush_writer_locked();
    char path[64];
    evlog_file_path(path, sizeof path, s_rd_seq);
    FILE *rf = fopen(path, "rb");
    if (rf == NULL) { xSemaphoreGive(s_mtx); return ESP_ERR_NOT_FOUND; }
    if (fseek(rf, s_rd_off, SEEK_SET) != 0 ||
        fgets(s_line, sizeof s_line, rf) == NULL) {
        fclose(rf);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    fclose(rf);

    size_t len = strlen(s_line);
    if (len == 0 || s_line[len - 1] != '\n') {
        /* Torn/over-long record: claim's own skip logic owns those cases. */
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* Fail closed if the caller's id is not still exactly at the frontier. */
    if ((int64_t)strtoll(s_line, NULL, 10) != measure_id) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* Archive FIRST — the record is only skipped once it is durably elsewhere.
     * If the append fails, stay put and let the caller retry next cycle. */
    FILE *qf = fopen(EVLOG_QUARANTINE, "a");
    if (qf == NULL) { xSemaphoreGive(s_mtx); return ESP_FAIL; }
    size_t w = fwrite(s_line, 1, len, qf);
    fflush(qf);
    fsync(fileno(qf));
    fclose(qf);
    if (w != len) { xSemaphoreGive(s_mtx); return ESP_FAIL; }

    /* Advance past exactly the cursor record without pretending it had a PUBACK.
     * If it was already claimed as slot zero, remove only that slot; later ACKed
     * slots may then form a real contiguous prefix and advance normally. */
    if (s_window_count > 0) {
        s_rd_seq = s_window[0].seq;
        s_rd_off = s_window[0].off + s_window[0].len;
        evlog_window_pop_front_locked();
    } else {
        s_rd_off += (long)len;
    }
    if (s_pending > 0) s_pending--;
    evlog_advance_acked_prefix_locked();
    evlog_normalize_cursor_locked();
    evlog_persist_cursor_locked();
    s_acks_since_persist = 0;
    bool reset_pending = s_reset_pending;
    s_reset_pending = false;
    xSemaphoreGive(s_mtx);

    if (reset_pending && s_reset_notifier != NULL) s_reset_notifier();

    ESP_LOGW(TAG, "quarantined event id=%lld (%u B) -> %s — cursor advanced, drain unblocked",
             (long long)measure_id, (unsigned)len, EVLOG_QUARANTINE);
    return ESP_OK;
}

static esp_err_t event_log_rewind_impl(uint32_t seq, uint32_t *out_seq, int64_t *out_pending)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_available) { xSemaphoreGive(s_mtx); return ESP_ERR_NOT_SUPPORTED; }

    /* Clamp the target to the files actually on the card. Drained files were
     * deleted, so the oldest present file (min_seq) is as far back as we can go;
     * seq==0 means "the oldest file" (re-publish everything still on the card). */
    uint32_t min_seq = 0, max_seq = 0;
    if (!evlog_scan_range_locked(&min_seq, &max_seq)) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;            /* no log files to rewind to */
    }
    uint32_t target = (seq == 0) ? min_seq : seq;
    if (target < min_seq) target = min_seq;
    if (target > max_seq) target = max_seq;
    /* Rewind only: never advance the cursor forward, which would skip still-pending
     * records and orphan their files (they're deleted only as the reader passes
     * them). A forward request is clamped back to the current cursor file. */
    if (target > s_rd_seq) target = s_rd_seq;

    s_rd_seq = target;
    s_rd_off = 0;
    /* Rewind abandons every RAM-only claim. device_commands_abort_inflight()
     * clears the peer msg-id table and completion queue before the CLI invokes
     * this path, so late PUBACKs cannot address the rebuilt window. */
    evlog_window_clear_locked();
    evlog_persist_cursor_locked();

    bool capped = false;
    s_pending = evlog_scan_pending_locked(&capped);
    int64_t pending = s_pending;

    if (out_seq)     *out_seq     = s_rd_seq;
    if (out_pending) *out_pending = pending;
    xSemaphoreGive(s_mtx);

    ESP_LOGW(TAG, "cursor rewound to seq=%u off=0 — %lld%s record(s) pending, will re-publish",
             (unsigned)target, (long long)pending, capped ? "+" : "");
    return ESP_OK;
}

esp_err_t event_log_cursor_info(uint32_t *rd_seq, uint32_t *rd_off, uint32_t *tail_seq)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (rd_seq)   *rd_seq   = s_rd_seq;
    if (rd_off)   *rd_off   = (uint32_t)s_rd_off;
    if (tail_seq) *tail_seq = s_tail_seq;
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

esp_err_t event_log_health(evlog_health_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    out->available     = s_available;
    out->write_full    = s_write_full;
    out->pending       = s_pending;
    out->next_id       = s_next_id;
    out->last_acked_id = s_last_acked_id;
    out->skipped       = s_skipped;
    out->dropped       = s_dropped;
    out->rd_seq        = s_rd_seq;
    out->tail_seq      = s_tail_seq;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* ── public entry points: SD RW-gate wrappers ─────────────────────────────
 * Every op that touches the FATFS volume runs between sdcard_io_begin()/io_end() so
 * an unmount cannot free the volume mid-op (audit F1/R-5). The begin/end are 1:1 by
 * construction — exactly one begin, exactly one end, NO branch between — so no return
 * path inside the *_impl (which keep all their own s_mtx handling) can leak a ref.
 * The ref spans the brief s_mtx wait too, which only delays a teardown by ms. */
esp_err_t event_log_store_event(const measurement_event_desc_t *desc)
{
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!sdcard_io_begin()) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t rc = event_log_store_impl(desc);
    sdcard_io_end();
    return rc;
}

esp_err_t event_log_claim_next_event(measurement_event_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!sdcard_io_begin()) { memset(out, 0, sizeof *out); return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t rc = event_log_claim_impl(out);
    sdcard_io_end();
    return rc;
}

esp_err_t event_log_quarantine_event(int64_t measure_id)
{
    if (!sdcard_io_begin()) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t rc = event_log_quarantine_impl(measure_id);
    sdcard_io_end();
    return rc;
}

esp_err_t event_log_rewind(uint32_t seq, uint32_t *out_seq, int64_t *out_pending)
{
    if (!sdcard_io_begin()) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t rc = event_log_rewind_impl(seq, out_seq, out_pending);
    sdcard_io_end();
    return rc;
}

/* ── fn getters ──────────────────────────────────────────────────────── */

measurement_next_id_fn            event_log_get_next_id_fn(void)            { return event_log_next_id; }
measurement_store_event_fn        event_log_get_store_event_fn(void)        { return event_log_store_event; }
measurement_claim_next_event_fn   event_log_get_claim_next_event_fn(void)   { return event_log_claim_next_event; }
measurement_mark_event_synced_fn  event_log_get_mark_event_synced_fn(void)  { return event_log_mark_event_synced; }
measurement_mark_event_pending_fn event_log_get_mark_event_pending_fn(void) { return event_log_mark_event_pending; }
measurement_quarantine_fn         event_log_get_quarantine_fn(void)         { return event_log_quarantine_event; }
measurement_db_stats_fn           event_log_get_db_stats_fn(void)           { return event_log_db_stats; }

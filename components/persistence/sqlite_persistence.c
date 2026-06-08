#include "sqlite_persistence.h"
#include "sd_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sqlite3.h"

#define TAG "sqlite_persist"
#define DB_PATH "/sdcard/measurements.db"
#define SCHEMA_VERSION 3

static sqlite3 *s_db = NULL;
static SemaphoreHandle_t s_mtx = NULL;
static StaticSemaphore_t s_mtx_storage;
static bool s_db_available = false;
static int64_t s_next_id = 1;

static esp_err_t db_open_and_configure(void);

/* ── helpers ─────────────────────────────────────────────────────────── */

static void increment_nvs_counter(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t n = 0;
        nvs_get_u32(h, key, &n);
        nvs_set_u32(h, key, n + 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static esp_err_t exec_pragma(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "PRAGMA failed: %s — %s", sql, err ? err : "");
        sqlite3_free(err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int integrity_cb(void *arg, int ncols, char **values, char **names)
{
    (void)names;
    bool *ok = (bool *)arg;
    if (ncols > 0 && values[0] && strcmp(values[0], "ok") != 0) *ok = false;
    return 0;
}

static char *dup_text_col(sqlite3_stmt *st, int col)
{
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return NULL;
    const unsigned char *t = sqlite3_column_text(st, col);
    if (!t) return NULL;
    size_t n = (size_t)sqlite3_column_bytes(st, col);
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, t, n);
    s[n] = '\0';
    return s;
}

static int64_t db_get_max_measure_id(void)
{
    if (!s_db) return 0;
    sqlite3_stmt *st = NULL;
    int64_t m = 0;
    if (sqlite3_prepare_v2(s_db, "SELECT MAX(measure_id) FROM events;", -1, &st, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(st) == SQLITE_ROW) m = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return m;
}

static esp_err_t db_open_and_configure(void)
{
    int rc = sqlite3_open(DB_PATH, &s_db);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "sqlite3_open failed: %s", sqlite3_errmsg(s_db));
        s_db = NULL;
        return ESP_FAIL;
    }

    bool ok = true;
    char *err = NULL;
    rc = sqlite3_exec(s_db, "PRAGMA integrity_check;", integrity_cb, &ok, &err);
    sqlite3_free(err);
    if (!ok || rc != SQLITE_OK) {
        ESP_LOGE(TAG, "DB integrity check failed, recovering");
        sqlite3_close_v2(s_db);
        s_db = NULL;
        remove(DB_PATH ".corrupt");          /* rename() fails if target exists */
        remove(DB_PATH "-wal.corrupt");
        remove(DB_PATH "-journal.corrupt");
        rename(DB_PATH, DB_PATH ".corrupt");
        rename(DB_PATH "-wal", DB_PATH "-wal.corrupt");
        rename(DB_PATH "-journal", DB_PATH "-journal.corrupt");
        increment_nvs_counter("db_corrupt_cnt");
        if (sqlite3_open(DB_PATH, &s_db) != SQLITE_OK) {
            s_db = NULL;
            return ESP_FAIL;
        }
    }

    sqlite3_extended_result_codes(s_db, 1);
    /* journal_mode=MEMORY: keep the rollback journal in RAM instead of creating
     * and truncating a -journal FILE on every transaction. That per-commit file
     * churn is a primary FAT-corruption source on consumer SD cards, and the
     * extra open file handle is a suspected leak on the corrupt path. Trade-off:
     * a power loss mid-commit can corrupt the DB (recovered on reboot via the
     * integrity check) — acceptable since events are also published to MQTT.
     * synchronous=NORMAL fsyncs far less often than FULL. */
    exec_pragma(s_db, "PRAGMA locking_mode = EXCLUSIVE;");
    exec_pragma(s_db, "PRAGMA journal_mode = MEMORY;");
    exec_pragma(s_db, "PRAGMA synchronous = NORMAL;");
    exec_pragma(s_db, "PRAGMA page_size = 4096;");
    /* -16 = 16 KiB page cache. The events DB is tiny and write-mostly, so a big
     * cache buys little; the freed ~48 KiB of internal heap is needed elsewhere
     * (Wi-Fi/TLS + the MQTT publish, which cJSON-parses the whole payload). */
    exec_pragma(s_db, "PRAGMA cache_size = -16;");
    exec_pragma(s_db, "PRAGMA temp_store = MEMORY;");

    const char *create =
        "CREATE TABLE IF NOT EXISTS events ("
        "measure_id  INTEGER PRIMARY KEY, "
        "device      TEXT, "
        "sensor      TEXT    NOT NULL, "
        "start_ticks INTEGER NOT NULL, "
        "end_ticks   INTEGER NOT NULL, "
        "metadata    TEXT, "
        "payload     TEXT    NOT NULL, "
        "sync_state  INTEGER NOT NULL DEFAULT 0);";
    err = NULL;
    rc = sqlite3_exec(s_db, create, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "CREATE events failed: %s", err ? err : "");
        sqlite3_free(err);
        sqlite3_close_v2(s_db);
        s_db = NULL;
        return ESP_FAIL;
    }
    sqlite3_free(err);

    err = NULL;
    sqlite3_exec(s_db, "CREATE INDEX IF NOT EXISTS idx_events_sync ON events(sync_state, measure_id);",
                 NULL, NULL, &err);
    sqlite3_free(err);

    char pragma[64];
    snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d;", SCHEMA_VERSION);
    exec_pragma(s_db, pragma);

    /* Crash recovery: rows left INFLIGHT by an interrupted session → PENDING. */
    err = NULL;
    rc = sqlite3_exec(s_db, "UPDATE events SET sync_state = 0 WHERE sync_state = 1;", NULL, NULL, &err);
    if (rc == SQLITE_OK) {
        int n = sqlite3_changes(s_db);
        if (n > 0) ESP_LOGW(TAG, "Crash recovery: reset %d INFLIGHT rows to PENDING", n);
    }
    sqlite3_free(err);

    int64_t mx = db_get_max_measure_id();
    if (mx + 1 > s_next_id) s_next_id = mx + 1;
    return ESP_OK;
}

/* ── public API ──────────────────────────────────────────────────────── */

esp_err_t sqlite_persistence_init(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_storage);
    if (s_mtx == NULL) return ESP_ERR_NO_MEM;

    if (sdcard_is_mounted()) {
        sqlite3_initialize();
        if (db_open_and_configure() == ESP_OK) {
            s_db_available = true;
            ESP_LOGI(TAG, "events DB ready (schema v%d), next measure_id = %lld",
                     (int)SCHEMA_VERSION, (long long)s_next_id);
        } else {
            ESP_LOGW(TAG, "SQLite unavailable");
        }
    } else {
        ESP_LOGW(TAG, "SD not mounted — persistence offline");
    }
    return ESP_OK;
}

esp_err_t sqlite_persistence_on_sd_lost(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_db) { sqlite3_close_v2(s_db); s_db = NULL; }
    s_db_available = false;
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "SD lost — DB closed");
    return ESP_OK;
}

esp_err_t sqlite_persistence_on_sd_restored(void)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (!s_db_available) {
        sqlite3_initialize();
        err = db_open_and_configure();
        s_db_available = (err == ESP_OK);
    }
    xSemaphoreGive(s_mtx);
    if (s_db_available) ESP_LOGI(TAG, "SD restored — DB reopened");
    return err;
}

esp_err_t sqlite_persistence_next_id(int64_t *out_id)
{
    if (out_id == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out_id = s_next_id++;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* Runtime corruption recovery (caller MUST hold s_mtx). The on-disk image went
 * malformed mid-operation (SQLITE_CORRUPT/NOTADB) — close it, archive to
 * `.corrupt` (clearing any prior archive first; FATFS rename() fails if the
 * target exists), and recreate a fresh DB so writes resume instead of failing
 * until reboot. close_v2 (not close) is used so the connection + its FATFS file
 * cache are always reclaimed even if SQLite considers it busy — plain close()
 * leaks them on a corrupt connection, which death-spiralled to OOM. The corrupt
 * DB's un-synced rows are lost (unavoidable). */
static void db_recover_locked(void)
{
    int crc = SQLITE_OK;
    if (s_db) { crc = sqlite3_close_v2(s_db); s_db = NULL; }
    ESP_LOGE(TAG, "DB corrupt at runtime (close=%d, free heap %u) — archiving + recreating",
             crc, (unsigned)esp_get_free_heap_size());
    remove(DB_PATH ".corrupt");
    remove(DB_PATH "-journal.corrupt");
    remove(DB_PATH "-wal.corrupt");
    rename(DB_PATH, DB_PATH ".corrupt");
    rename(DB_PATH "-journal", DB_PATH "-journal.corrupt");
    rename(DB_PATH "-wal", DB_PATH "-wal.corrupt");
    increment_nvs_counter("db_corrupt_cnt");
    s_db_available = (db_open_and_configure() == ESP_OK);
    if (!s_db_available) {
        ESP_LOGE(TAG, "DB reopen after corruption failed");
    }
}

esp_err_t sqlite_persistence_store_event(int64_t measure_id, const char *device, const char *sensor,
                                         int64_t start_ms, int64_t end_ms,
                                         const char *metadata_json, const char *payload_json)
{
    if (sensor == NULL || payload_json == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_db_available || s_db == NULL) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *sql =
        "INSERT OR REPLACE INTO events "
        "(measure_id, device, sensor, start_ticks, end_ticks, metadata, payload, sync_state) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 0);";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "store_event prepare failed: %s", sqlite3_errmsg(s_db));
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    sqlite3_bind_int64(st, 1, measure_id);
    if (device == NULL || device[0] == '\0') sqlite3_bind_null(st, 2);
    else sqlite3_bind_text(st, 2, device, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, sensor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, start_ms);
    sqlite3_bind_int64(st, 5, end_ms);
    if (metadata_json == NULL || metadata_json[0] == '\0') sqlite3_bind_null(st, 6);
    else sqlite3_bind_text(st, 6, metadata_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 7, payload_json, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        ESP_LOGW(TAG, "store_event step failed (%d) for id %lld", rc, (long long)measure_id);
        /* On-disk corruption: self-heal so the NEXT store works. Rate-limited to
         * at most once per 20 s — recreating in a tight loop hammers an already-
         * struggling card and accumulates the per-recovery cost. Between attempts
         * the corrupt connection is reused (no churn), and stores just fail.
         * (Extended codes share the low 8 bits with the base code.) */
        if ((rc & 0xFF) == SQLITE_CORRUPT || (rc & 0xFF) == SQLITE_NOTADB) {
            static TickType_t s_last_recover_tick = 0;
            TickType_t now = xTaskGetTickCount();
            if (s_last_recover_tick == 0 || (now - s_last_recover_tick) >= pdMS_TO_TICKS(20000)) {
                s_last_recover_tick = now;
                db_recover_locked();
            }
        }
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t sqlite_persistence_claim_next_event(measurement_event_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_db_available || s_db == NULL) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }

    char *err = NULL;
    if (sqlite3_exec(s_db, "BEGIN IMMEDIATE;", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    sqlite3_free(err);

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s_db,
        "SELECT measure_id, device, sensor, start_ticks, end_ticks, metadata, payload "
        "FROM events WHERE sync_state = 0 ORDER BY measure_id LIMIT 1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(s_db, "ROLLBACK;", NULL, NULL, NULL);
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        sqlite3_exec(s_db, "ROLLBACK;", NULL, NULL, NULL);
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_FOUND;
    }

    out->measure_id = sqlite3_column_int64(st, 0);
    if (sqlite3_column_type(st, 1) != SQLITE_NULL) {
        const unsigned char *d = sqlite3_column_text(st, 1);
        if (d) strncpy(out->device, (const char *)d, sizeof(out->device) - 1);
    }
    const unsigned char *s = sqlite3_column_text(st, 2);
    if (s) strncpy(out->sensor, (const char *)s, sizeof(out->sensor) - 1);
    out->start_ticks_ms = sqlite3_column_int64(st, 3);
    out->end_ticks_ms   = sqlite3_column_int64(st, 4);
    out->metadata_json  = dup_text_col(st, 5);
    out->payload_json   = dup_text_col(st, 6);
    sqlite3_finalize(st);

    if (out->payload_json == NULL) {  /* alloc failure — don't consume the row */
        sqlite3_exec(s_db, "ROLLBACK;", NULL, NULL, NULL);
        xSemaphoreGive(s_mtx);
        measurement_event_free(out);
        return ESP_ERR_NO_MEM;
    }

    rc = sqlite3_prepare_v2(s_db, "UPDATE events SET sync_state = 1 WHERE measure_id = ?;", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, out->measure_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    err = NULL;
    sqlite3_exec(s_db, "COMMIT;", NULL, NULL, &err);
    sqlite3_free(err);
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

static esp_err_t mark_event(int64_t measure_id, int new_state)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_db_available || s_db == NULL) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NOT_SUPPORTED;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(s_db,
        "UPDATE events SET sync_state = ? WHERE measure_id = ? AND sync_state = 1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    sqlite3_bind_int  (st, 1, new_state);
    sqlite3_bind_int64(st, 2, measure_id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    xSemaphoreGive(s_mtx);
    return (rc == SQLITE_DONE) ? ESP_OK : ESP_FAIL;
}

esp_err_t sqlite_persistence_mark_event_synced(int64_t measure_id)
{
    return mark_event(measure_id, (int)MEASUREMENT_SYNC_SYNCED);
}

esp_err_t sqlite_persistence_mark_event_pending(int64_t measure_id)
{
    return mark_event(measure_id, (int)MEASUREMENT_SYNC_PENDING);
}

esp_err_t sqlite_persistence_db_stats(bool *available, int64_t *total,
                                      int64_t *pending, int64_t *next_id)
{
    if (s_mtx == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    bool    avail = (s_db_available && s_db != NULL);
    int64_t tot = 0, pend = 0;
    if (avail) {
        sqlite3_stmt *st = NULL;
        /* One pass: total rows + count of those not yet synced. SUM over zero
         * rows yields NULL, which sqlite3_column_int64 reads back as 0. */
        const char *sql = "SELECT COUNT(*), "
                          "SUM(CASE WHEN sync_state <> 2 THEN 1 ELSE 0 END) FROM events;";
        if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                tot  = sqlite3_column_int64(st, 0);
                pend = sqlite3_column_int64(st, 1);
            }
            sqlite3_finalize(st);
        }
    }
    if (available) *available = avail;
    if (total)     *total     = tot;
    if (pending)   *pending   = pend;
    if (next_id)   *next_id   = s_next_id;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* ── fn getters ──────────────────────────────────────────────────────── */

measurement_next_id_fn            sqlite_persistence_get_next_id_fn(void)            { return sqlite_persistence_next_id; }
measurement_store_event_fn        sqlite_persistence_get_store_event_fn(void)        { return sqlite_persistence_store_event; }
measurement_claim_next_event_fn   sqlite_persistence_get_claim_next_event_fn(void)   { return sqlite_persistence_claim_next_event; }
measurement_mark_event_synced_fn  sqlite_persistence_get_mark_event_synced_fn(void)  { return sqlite_persistence_mark_event_synced; }
measurement_mark_event_pending_fn sqlite_persistence_get_mark_event_pending_fn(void) { return sqlite_persistence_mark_event_pending; }
measurement_db_stats_fn           sqlite_persistence_get_db_stats_fn(void)           { return sqlite_persistence_db_stats; }

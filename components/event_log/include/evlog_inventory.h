#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read-only inventory of the SD archive written by event_log_archive_to_sd.
 *
 * Answers "what does this device still hold on its card?" without touching the
 * event store, the cursor or the SD contents. Scans /sdcard/archive/arc-*.log
 * (verbatim copies of fully-acknowledged event_log files, format v2: nine
 * tab-separated fields, newline framed) and reports counts, id and capture-time
 * bounds, and how many records fall inside an optional id/time window. The
 * legacy /sdcard/events import directory is only counted.
 *
 * Pure C: no FreeRTOS, no SD gating, no logging. The caller supplies hooks for
 * the FATFS read gate (sdcard_io_begin/end, bracketed 1:1 per directory listing
 * and per file) and for yielding between files. Host tests compile this file
 * against tests/host_stubs.
 */

/* Mirrors EVLOG_ARCHIVE_DIR / EVLOG_LEGACY_SD_DIR in event_log.c. */
#define EVLOG_INVENTORY_ARCHIVE_DIR "/sdcard/archive"
#define EVLOG_INVENTORY_LEGACY_DIR  "/sdcard/events"

#define EVLOG_INVENTORY_LIST_MAX   48     /* per-file rows carried in the reply */
#define EVLOG_INVENTORY_MAX_FILES  512    /* archive files sorted and scanned per call */
#define EVLOG_INVENTORY_HEAD_BYTES 1024   /* bytes of each line read for id/start_ms */
/* Same floor as clock_trust / sync_runner: 2024-01-01T00:00:00Z in ms. Records
 * captured before time sync carry 1970-era stamps and are counted separately. */
#define EVLOG_INVENTORY_CLOCK_FLOOR_MS 1704067200000LL

typedef struct {
    int64_t from_id;    /* 0 = unbounded */
    int64_t to_id;      /* 0 = unbounded */
    int64_t from_ms;    /* 0 = unbounded, epoch ms on start_ms */
    int64_t to_ms;      /* 0 = unbounded */
} evlog_inventory_window_t;

typedef struct {
    int64_t  name_id;    /* the <id> in arc-<id>.log */
    int64_t  first_id;   /* id of the first complete record, 0 if none */
    int64_t  last_id;    /* id of the last complete record, 0 if none */
    uint32_t records;
    uint32_t in_window;
    uint32_t bytes;
} evlog_inventory_file_t;

typedef struct {
    bool     archive_dir_present;
    bool     sd_lost;            /* an io_begin refused mid-scan; results are partial */
    bool     files_truncated;    /* more than EVLOG_INVENTORY_MAX_FILES matched */
    bool     list_truncated;     /* more files than EVLOG_INVENTORY_LIST_MAX rows */
    uint32_t files;              /* arc-<digits>.log entries found */
    uint32_t other_entries;      /* entries in the archive dir with another name */
    uint32_t open_failed;
    uint32_t records;            /* complete (newline-terminated) lines */
    uint32_t torn;               /* files ending in a line without newline */
    uint32_t unparsed;           /* complete lines whose head lacked id/start_ms */
    uint32_t in_window;
    uint32_t pre_clock_floor;    /* records with start_ms before 2024-01-01 */
    uint64_t bytes;
    int64_t  min_id, max_id;                 /* over parsed records, 0 if none */
    int64_t  min_start_ms, max_start_ms;     /* over parsed records, 0 if none */
    uint32_t listed;
    evlog_inventory_file_t list[EVLOG_INVENTORY_LIST_MAX];
    uint32_t legacy_files;       /* ev-<digits>.log entries under the legacy dir */
} evlog_inventory_t;

typedef struct {
    bool (*io_begin)(void);      /* NULL = always allowed */
    void (*io_end)(void);
    void (*yield)(void);         /* called between files; NULL = none */
} evlog_inventory_hooks_t;

/* Scan `archive_dir` (and count `legacy_dir`, which may be NULL). `win` may be
 * NULL for no window. `out` is fully overwritten. Returns ESP_OK even when the
 * directory is absent (archive_dir_present=false); ESP_ERR_INVALID_ARG on NULL
 * archive_dir/out; ESP_ERR_NO_MEM if the file-id table cannot be allocated. */
esp_err_t evlog_inventory_scan(const char *archive_dir, const char *legacy_dir,
                               const evlog_inventory_window_t *win,
                               const evlog_inventory_hooks_t *hooks,
                               evlog_inventory_t *out);

/* Render the reply published on the status topic. `id` and `device_id` are
 * copied with JSON escaping and a 64-byte bound (the id is MQTT input). Returns
 * the number of bytes written (excluding the NUL), or -1 if `cap` was too small;
 * on -1 the buffer holds a truncated, invalid document and must not be sent. */
int evlog_inventory_render_json(const evlog_inventory_t *inv,
                                const evlog_inventory_window_t *win,
                                const char *id, const char *device_id, const char *firmware,
                                bool sd_mounted, uint64_t store_free_bytes,
                                int64_t pending, int64_t last_acked_id, int64_t next_id,
                                uint32_t scan_ms, bool include_list,
                                char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#ifndef AMBYTE_EVENT_LOG_H
#define AMBYTE_EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "persistence_port.h"

/* Internal event store volume: littlefs on the (previously unused) 9.4 MiB
 * "storage" partition. app_main mounts it at EVSTORE_MOUNT before
 * event_log_init; the partition label predates this use and MUST NOT change —
 * partition tables cannot be OTA'd, and esp_littlefs finds it by label with
 * SUBTYPE_ANY, so the legacy `fat` subtype byte is harmless. */
#define EVSTORE_MOUNT     "/evstore"
#define EVSTORE_PARTITION "storage"

/* Compile-time storage limits shared with the AMBIT producer and MQTT publisher.
 * event_log_init selects NORMAL when PSRAM enumerates and FALLBACK otherwise;
 * the active runtime cap is kept private to event_log.c. A record whose byte
 * length is >= the selected RECORD_CAP is refused, leaving the line-buffer guard
 * available for framing/NUL safety. */
#define EVLOG_LINE_CAP_NORMAL       65568U
#define EVLOG_LINE_CAP_FALLBACK     12288U
#define EVLOG_RECORD_GUARD_BYTES    16U
#define EVLOG_RECORD_CAP_NORMAL     (EVLOG_LINE_CAP_NORMAL - EVLOG_RECORD_GUARD_BYTES)
#define EVLOG_RECORD_CAP_FALLBACK   (EVLOG_LINE_CAP_FALLBACK - EVLOG_RECORD_GUARD_BYTES)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Append-only event log behind the `persistence_port.h` interface.
 *
 * Why append-only: the workload is a store-and-forward FIFO, not a relational
 * query — and in-place page rewrites (the SQLite bench experiment; never shipped)
 * are hostile to flash storage. Why INTERNAL littlefs instead of the SD card
 * (since the store moved off /sdcard/events): littlefs is copy-on-write and
 * power-loss-safe, so a brownout can never tear record framing or the filesystem
 * itself — the FAT-metadata corruption class PR #27 documented is structurally
 * gone from the measurement path, and the device keeps measuring, storing, and
 * publishing with no SD card at all.
 *
 * Storage model: EVSTORE_MOUNT/events holds rotating files ev-000001.log, …
 * (monotonic seq). One newline-terminated, tab-delimited record per event
 * (format v2, 9 fields):
 *   <measure_id>\t<channel>\t<device>\t<tag>\t<cmd_raw>\t<start_ms>\t<end_ms>\t<metadata>\t<payload>\n
 * The record layout is deliberately schema-neutral: new firmware writes the
 * complete canonical ambit.trace/3, ambyte.telemetry/1, or ambit.device/1 object
 * in the payload column and leaves metadata empty. New firmware also keeps a
 * permanent lossless v2 trace fallback for missing v3 prerequisites or an
 * unrepresentable time model; those rows use the same split metadata/payload
 * columns as old v2 rows and continue through the legacy publisher unchanged.
 * The producer's binding maximum is 65,192 B (62,999 payload + 1,535 metadata
 * + 543 command + 113 fixed header + 2 framing), leaving 360 B below the
 * exported 65,552-B normal record cap.
 * The read cursor and a next_id high-water mark live in NVS. mark_synced
 * advances the cursor. RETENTION: a fully-drained (100% synced) rotated file is
 * kept for the bulk SD archive rather than deleted; when free space runs low the
 * oldest synced files are EVICTED first — unsynced records always outrank synced
 * archive copies (a dead SD + slow uplink must never make the store refuse new
 * measurements while old synced data holds the space).
 */
esp_err_t event_log_init(void);

/* Register the composition-root hook that clears the MQTT correlation window
 * after a successful SD reopen rebuilds this component from the durable cursor.
 * NULL clears it. The callback runs after event_log releases its mutex. */
void event_log_set_reset_notifier(void (*fn)(void));

/* ── SD interchange (keeper-task driven; the store itself never needs the SD) ──
 * archive_pending: true once EVLOG_ARCHIVE_EVERY_N stores accumulated and the SD
 * is mounted. archive_to_sd: copy every fully-synced retained file to
 * /sdcard/archive/arc-<first_measure_id>.log in one burst, then free the internal
 * copies. import_sd_backlog: one-shot fleet migration — re-append records from
 * the legacy /sdcard/events backlog (oldest-first, verbatim, measure_ids kept so
 * duplicates dedup downstream), deleting each SD file only after its records are
 * fsync'd internally; imports at most max_files per call, returns files done.
 * free_bytes: free space on the internal store partition (STATUS telemetry). */
bool      event_log_archive_pending(void);
esp_err_t event_log_archive_to_sd(size_t *out_archived);
size_t    event_log_import_sd_backlog(size_t max_files);
esp_err_t event_log_free_bytes(uint64_t *out_free);

/* Pre-reboot power-safety drain (register once via esp_register_shutdown_handler).
 * Flushes + fsyncs the periodically-buffered tail, persists the read cursor, and
 * closes the tail file so a following sdcard_unmount() can finalize FATFS cleanly
 * instead of leaving a torn FAT/dir-entry metadata write. Bounded lock wait. */
esp_err_t event_log_prepare_shutdown(void);

/* Event store / claim / mark (see persistence_port.h for semantics). */
esp_err_t event_log_next_id(int64_t *out_id);
esp_err_t event_log_store_event(const measurement_event_desc_t *desc);
esp_err_t event_log_claim_next_event(measurement_event_t *out);
esp_err_t event_log_mark_event_synced(int64_t measure_id);
esp_err_t event_log_mark_event_pending(int64_t measure_id);

/* Poison-event escape (measurement_quarantine_fn): append the record at the
 * read cursor — which must carry `measure_id` — to EVSTORE_MOUNT/events/
 * quarantine.log, then advance the cursor past it. Skips only after a successful
 * archive write, so quarantined data is preserved (re-ingest manually if wanted). */
esp_err_t event_log_quarantine_event(int64_t measure_id);

/* Read-only stats (see measurement_db_stats_fn). *total mirrors *pending — the
 * publishable backlog. (Synced records are also physically retained until
 * archived/evicted, but they are dead weight for the publisher and are not
 * counted here.) */
esp_err_t event_log_db_stats(bool *available, int64_t *total,
                             int64_t *pending, int64_t *next_id);

/* Richer health snapshot for the TELEMETRY heartbeat — makes the silent-loss sites
 * observable in the field. `skipped` counts records/files the drain passed without
 * publishing (external delete, corrupt/over-long line, OOM-quarantine); `dropped`
 * counts records refused at store (too-large, storage-full, short-write);
 * `last_acked_id` is the end-to-end delivery high-water; `write_full` is the
 * storage-full-but-healthy state. */
typedef struct {
    bool     available;
    bool     write_full;
    int64_t  pending;
    int64_t  next_id;
    int64_t  last_acked_id;
    int64_t  skipped;
    int64_t  dropped;
    uint32_t rd_seq;
    uint32_t tail_seq;
} evlog_health_t;

esp_err_t event_log_health(evlog_health_t *out);

/* Rewind the read cursor to the start of file ev-<seq>.log so that record and all
 * newer ones revert to PENDING and re-publish. Pass seq=0 to rewind to the oldest
 * file still on the card (re-publish everything). The target is clamped to the
 * files actually present, the RAM claim window is abandoned, and the cursor
 * is persisted to NVS. Fills *out_seq (the clamped target) and *out_pending (a
 * floor if the count was capped); either may be NULL. Re-publish is at-least-once,
 * so records already delivered are re-sent and deduped downstream on measure_id. */
esp_err_t event_log_rewind(uint32_t seq, uint32_t *out_seq, int64_t *out_pending);

/* Diagnostic: report the read cursor position and the current tail file seq. Any
 * out-pointer may be NULL. */
esp_err_t event_log_cursor_info(uint32_t *rd_seq, uint32_t *rd_off, uint32_t *tail_seq);

/* Getters for function pointers (wired into device_commands_config_t). */
measurement_next_id_fn            event_log_get_next_id_fn(void);
measurement_store_event_fn        event_log_get_store_event_fn(void);
measurement_claim_next_event_fn   event_log_get_claim_next_event_fn(void);
measurement_mark_event_synced_fn  event_log_get_mark_event_synced_fn(void);
measurement_mark_event_pending_fn event_log_get_mark_event_pending_fn(void);
measurement_quarantine_fn         event_log_get_quarantine_fn(void);
measurement_db_stats_fn           event_log_get_db_stats_fn(void);

#ifdef __cplusplus
}
#endif

#endif

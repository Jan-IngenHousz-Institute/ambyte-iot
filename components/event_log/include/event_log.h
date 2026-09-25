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
#ifndef EVSTORE_MOUNT
#define EVSTORE_MOUNT     "/evstore"      /* guarded only for the host harness (check_constants.py) */
#endif
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
 *
 * SD OVERFLOW (docs/evq-sd-overflow.md): a rotated file may also live on SD —
 * unsent ones as a verified primary + mirror, so flash is a bounded buffer and
 * an outage longer than the ~17 h store no longer refuses measurements while
 * the card has room. The cursor offsets are identical on both media; the
 * segment index (evq_index.h, internal flash) records which copy exists, and
 * an indexed file is NEVER skipped because its card is missing.
 */
/* Boot-only ID allocator initialization; independent of filesystem/SD. Call
 * before producers start. event_log_init also invokes this idempotently. */
esp_err_t event_log_init_ids(void);
esp_err_t event_log_init(void);

/* Register the composition-root hook that clears the MQTT correlation window
 * after a successful SD reopen rebuilds this component from the durable cursor.
 * NULL clears it. The callback runs after event_log releases its mutex. */
void event_log_set_reset_notifier(void (*fn)(void));

/* ── SD overflow (keeper-task driven; the store itself never needs the SD) ──
 * Rotated files move to SD in bursts (every 1000 stores, or at once under flash
 * pressure) whether or not they are delivered: unsent files as a verified
 * primary (/sdcard/events, rollback-importable) + mirror (/sdcard/evq); a flash
 * copy is reclaimed only after both verify. Delivered files go to
 * /sdcard/archive. See docs/evq-sd-overflow.md.
 *
 * keeper_start: start the production keeper task (60 s period + wake on store
 * pressure / batch count / card mount / unpark). service: one keeper pass —
 * exposed for the host harness and diagnostics; the task calls it.
 * set_sd_parked: the low-battery guard parks the card (no SD operation at all
 * until unparked). free_bytes: free space on the internal store partition. */
esp_err_t event_log_sd_keeper_start(void);
esp_err_t event_log_sd_service(void);
void      event_log_sd_notify(void);
bool      event_log_sd_service_pending(void);
void      event_log_set_sd_parked(bool parked);
esp_err_t event_log_free_bytes(uint64_t *out_free);

/* ── Archive replay support (evlog_replay) ──
 * append_verbatim: re-append one already-framed v2 record line (e.g. read back
 * from /sdcard/archive) to the tail as PENDING, keeping its measure_id and
 * capture times; next_id stays above it. ESP_ERR_INVALID_ARG = malformed line
 * (skip), ESP_ERR_NO_MEM = store full (nothing written, retry later), ESP_FAIL =
 * media write failure. flush: fsync the tail (call after a chunk of appends,
 * before persisting replay progress). max_pending_id_in_range: highest pending
 * id inside [from_id, to_id] (to_id 0 = unbounded), for exact resume. */
esp_err_t event_log_append_verbatim(const char *line, size_t len, int64_t *out_id);
esp_err_t event_log_flush(void);
esp_err_t event_log_max_pending_id_in_range(int64_t from_id, int64_t to_id,
                                            int64_t *out_max, bool *out_capped);
/* collect_ids_in_range: every id in [from_id, to_id] present in any file still
 * on flash (pending, or synced but not yet archived/evicted), unsorted, at most
 * `cap`; *out_capped when the set is incomplete. The replay resume set. */
esp_err_t event_log_collect_ids_in_range(int64_t from_id, int64_t to_id, int64_t *ids,
                                         size_t cap, size_t *out_n, bool *out_capped);

/* Pre-reboot power-safety drain (register once via esp_register_shutdown_handler).
 * Flushes + fsyncs the periodically-buffered tail, persists the read cursor, and
 * closes the tail file so a following sdcard_unmount() can finalize FATFS cleanly
 * instead of leaving a torn FAT/dir-entry metadata write. Bounded lock wait. */
esp_err_t event_log_prepare_shutdown(void);

/* Event store / claim / mark (see persistence_port.h for semantics).
 * store_event: ESP_OK only once the record is fsync'd (ACCEPTED); refusals are
 * counted by reason in evlog_health_t. claim_next_event additionally returns
 * ESP_ERR_NOT_FINISHED when the delivery head is an indexed segment whose only
 * copies cannot be read right now (card absent/parked/swapped, or copies
 * missing/corrupt): the cursor WAITS in place — this is never "queue empty". */
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

/* SD view reported in health (sd_state). */
typedef enum {
    EVQ_SD_ABSENT = 0,       /* no card mounted */
    EVQ_SD_OK,
    EVQ_SD_LOST,             /* I/O-loss latch / failing card */
    EVQ_SD_PARKED,           /* low-battery guard parked it */
    EVQ_SD_FULL,             /* below the SD reserve — spooling paused */
    EVQ_SD_MISMATCH,         /* a different card while another CID holds obligations: parked, zero ops */
    EVQ_SD_BACKLOG_MISSING,  /* head segment has no readable copy anywhere */
    EVQ_SD_BACKLOG_CORRUPT,  /* head segment's copies all fail verification */
} evq_sd_state_t;

/* Why the delivery head waits (claim returned ESP_ERR_NOT_FINISHED). */
typedef enum {
    EVQ_BLOCK_NONE = 0,
    EVQ_BLOCK_SD_ABSENT,
    EVQ_BLOCK_SD_LOST,
    EVQ_BLOCK_SD_PARKED,
    EVQ_BLOCK_SD_MISMATCH,
    EVQ_BLOCK_BACKLOG_MISSING,
    EVQ_BLOCK_BACKLOG_CORRUPT,
} evq_block_t;

/* Why stores are refused (storage_blocked). */
typedef enum {
    EVQ_BLOCKED_NONE = 0,
    EVQ_BLOCKED_SD_UNAVAILABLE,      /* flash_full_sd_unavailable */
    EVQ_BLOCKED_SD_FULL,             /* flash_full_sd_full */
    EVQ_BLOCKED_SD_ERROR,            /* flash_full_sd_error */
    EVQ_BLOCKED_SD_MISMATCH,         /* flash_full_sd_mismatch */
    EVQ_BLOCKED_BACKLOG_WAITING,     /* flash_full_backlog_waiting */
    EVQ_BLOCKED_INDEX_CAP,           /* flash_full_index_cap */
    EVQ_BLOCKED_TRANSFER_PENDING,    /* flash_full_transfer_pending: SD fine, keeper has not freed space yet */
} evq_blocked_t;

typedef enum { EVQ_MEDIUM_NONE = 0, EVQ_MEDIUM_FLASH, EVQ_MEDIUM_SD } evq_medium_t;

/* Health snapshot for the TELEMETRY heartbeat and the `evlog` CLI — makes every
 * loss/blocked site observable in the field. Counts are exact when
 * pending_exact; otherwise pending is a FLOOR (unindexed pre-upgrade files, an
 * unvalidated cursor, or an unknown re-import remainder).
 *   pending            = flash_pending + sd_pending + reimport_pending
 *   deliverable_pending= what the drain can reach now (0 while the head waits)
 *   refused_*          = store results by reason (ESP_ERR_NO_MEM / ESP_FAIL /
 *                        ESP_ERR_INVALID_SIZE / unavailable)
 *   quarantined_*      = records passed WITHOUT an ACK after an intact copy was
 *                        fsync'd to quarantine.log (never accepted records)
 *   corrupt_detected   = post-commit corruption detected (never delivered)
 * `skipped`/`dropped`/`write_full` keep their pre-1.0.6 aggregate meaning. */
typedef struct {
    bool     available;
    bool     write_full;
    bool     pending_exact;
    bool     storage_blocked;
    int64_t  pending;
    int64_t  deliverable_pending;
    int64_t  flash_pending;
    int64_t  sd_pending;
    int64_t  reimport_pending;
    int64_t  next_id;
    int64_t  last_acked_id;
    int64_t  skipped;
    int64_t  dropped;
    int64_t  refused_full;
    int64_t  refused_media;
    int64_t  refused_too_large;
    int64_t  refused_unavailable;
    int64_t  quarantined_poison;
    int64_t  quarantined_malformed;
    int64_t  skipped_unindexed_gap;
    int64_t  corrupt_detected;
    uint32_t rd_seq;
    uint32_t tail_seq;
    uint8_t  sd_state;          /* evq_sd_state_t */
    uint8_t  head_block;        /* evq_block_t */
    uint8_t  blocked_reason;    /* evq_blocked_t */
    uint8_t  corrupt_medium;    /* evq_medium_t */
    uint32_t spool_files;
    uint32_t spool_errors;
    uint32_t mirror_used;
    uint32_t reclaimed_files;
    uint32_t archived_files;
    uint32_t reimported_files;
    uint32_t pressure_notifies;
    uint32_t sd_bursts;
    uint32_t index_segments;
    uint32_t index_cap;
} evlog_health_t;

/* Stable wire/CLI names (evq_render.c). */
const char *event_log_sd_state_name(uint8_t state);
const char *event_log_block_name(uint8_t block);
const char *event_log_blocked_reason_name(uint8_t reason);
const char *event_log_medium_name(uint8_t medium);

/* Render `h` as the `evlog` CLI text (key=value lines). Returns the length, or
 * -1 if it would not fit `cap` — never a truncated rendering. */
int evq_render_health_text(const evlog_health_t *h, char *buf, size_t cap);

esp_err_t event_log_health(evlog_health_t *out);

/* Rewind the read cursor to the start of file ev-<seq>.log so that record and all
 * newer ones revert to PENDING and re-publish. Pass seq=0 to rewind to the oldest
 * file the queue can still read (flash, or a SPOOLED/SD_ONLY copy on SD). The target is clamped to the
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

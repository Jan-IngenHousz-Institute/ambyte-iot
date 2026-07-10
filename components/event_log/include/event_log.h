#ifndef AMBYTE_EVENT_LOG_H
#define AMBYTE_EVENT_LOG_H

#include <stdint.h>

#include "esp_err.h"
#include "persistence_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Append-only event log — drop-in replacement for the SQLite persistence layer
 * behind the same `persistence_port.h` interface.
 *
 * Why: SQLite's in-place page + header rewrites on every commit corrupt FATFS/SD
 * under load. The workload is a store-and-forward FIFO, not a relational query,
 * so an append-only log is both the right tool and the proven-robust one (the
 * prior firmware logged to a TXT file on the same card for months without
 * corruption; the Step-0 spike reproduced that result — see
 * docs/append-log-persistence-plan.md).
 *
 * Storage model: /sdcard/events/ holds rotating files ev-000001.log, … (monotonic
 * seq). One newline-terminated, tab-delimited record per event (format v2,
 * 9 fields; v1 7-field records are skipped as malformed — planned wipe/drain
 * at the v2 deploy, see docs/payload-v2-plan.md):
 *   <measure_id>\t<channel>\t<device>\t<tag>\t<cmd_raw>\t<start_ms>\t<end_ms>\t<metadata>\t<payload>\n
 * The tail file is appended to (store) with PERIODIC flush; a fresh read handle
 * after fsync serves claims (the read+append handshake the per-file-cache FATFS
 * build needs). The read cursor and a next_id high-water mark live in NVS
 * (internal flash), keeping the SD to appends only. mark_synced advances the
 * cursor; a fully-drained rotated file is deleted to reclaim space. The reader
 * skips any record it can't parse, so a power-loss-torn final record needs no
 * boot-time repair.
 */
esp_err_t event_log_init(void);

/* Called by the SD hot-plug monitor when the card is pulled/reinserted. */
esp_err_t event_log_on_sd_lost(void);
esp_err_t event_log_on_sd_restored(void);

/* Event store / claim / mark (see persistence_port.h for semantics). */
esp_err_t event_log_next_id(int64_t *out_id);
esp_err_t event_log_store_event(const measurement_event_desc_t *desc);
esp_err_t event_log_claim_next_event(measurement_event_t *out);
esp_err_t event_log_mark_event_synced(int64_t measure_id);
esp_err_t event_log_mark_event_pending(int64_t measure_id);

/* Poison-event escape (measurement_quarantine_fn): append the record at the
 * read cursor — which must carry `measure_id` — to /sdcard/events/quarantine.log,
 * then advance the cursor past it. Skips only after a successful archive write,
 * so quarantined data is preserved on the card (re-ingest manually if wanted). */
esp_err_t event_log_quarantine_event(int64_t measure_id);

/* Read-only stats (see measurement_db_stats_fn). *total == *pending: every
 * record physically present in the log is not-yet-synced (synced records are
 * dropped as the cursor advances and drained files are deleted). */
esp_err_t event_log_db_stats(bool *available, int64_t *total,
                             int64_t *pending, int64_t *next_id);

/* Rewind the read cursor to the start of file ev-<seq>.log so that record and all
 * newer ones revert to PENDING and re-publish. Pass seq=0 to rewind to the oldest
 * file still on the card (re-publish everything). The target is clamped to the
 * files actually present, the claimed in-flight slot is abandoned, and the cursor
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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tee WARN/ERROR ESP-IDF log lines to a rotating set of text files on the SD
 * card, in addition to the console (which still shows every level). Call once,
 * as early as possible in app_main (the FreeRTOS scheduler must already be
 * running), so early WARN/ERROR are captured.
 *
 * WARN/ERROR-only is deliberate: continuous INFO-level logging to a consumer SD
 * card that also holds the events DB corrupted the shared FAT. The file is now
 * low-volume and writes nothing while idle.
 *
 * Design: the esp_log vprintf hook formats each line (RTC wall-clock prefix,
 * ANSI colour stripped) into an in-RAM ring buffer — it never touches the SD
 * card, so logging never blocks. A low-priority writer task drains the ring to
 * /sdcard/logs/ambyte.log, rotating at SD_LOGGER_FILE_BYTES across
 * SD_LOGGER_MAX_FILES files (current + rotated). While the card is absent the
 * ring holds lines (dropping the newest on overflow) and flushes once it
 * mounts; on a mid-run pull the file is closed and reopened on reinsertion. */
esp_err_t sd_logger_init(void);

/* Low-battery park handshake (used by app_main's persistence guard around a
 * deliberate unmount). pause() signals the writer task to drain the RAM ring to
 * the file, fsync + CLEANLY close it, then hold — buffering to RAM only — until
 * resume(); blocks up to ~1 s for the close so the caller may unmount right after.
 * Unlike prepare_shutdown() the writer task survives and resumes normal service
 * (reopening the file) after resume(), so a park can recur nightly without
 * leaking handles. Both are no-ops before sd_logger_init(). */
void sd_logger_pause(void);
void sd_logger_resume(void);

/* Pre-reboot power-safety flush (register once via esp_register_shutdown_handler,
 * before sdcard_unmount()). Signals the writer task to drain the RAM ring to the
 * current file, fsync + close it, and park; blocks up to ~1 s for that to finish.
 * Leaves the log file's FATFS metadata finalizable by a following unmount. */
void sd_logger_prepare_shutdown(void);

/* Diagnostics snapshot (e.g. for a CLI command). Any out-pointer may be NULL.
 *  active     : the log file is currently open and being written
 *  buffered   : bytes waiting in the RAM ring buffer
 *  dropped    : total bytes dropped on ring overflow since boot
 *  file_bytes : size of the current (active) log file */
void sd_logger_stats(bool *active, size_t *buffered, size_t *dropped, size_t *file_bytes);

/* Byte-exact accounting (2026-09 write-integrity audit). Every byte a log call
 * produced is, at any moment, in exactly one place: intact in a log file, still
 * in the RAM ring (buffered_bytes), or in one of the buckets below. Intentional
 * retention (the oldest rotated file deleted) is kept apart from fault loss.
 *   dropped_ring_bytes / dropped_records : whole records refused by a full ring
 *   dropped_rotate_blocked_bytes         : popped while rotation was blocked
 *                                          (file at its 2x ceiling or quarantined)
 *   dropped_unavailable_bytes            : popped while no log file could be opened
 *   rolled_back_bytes                    : written, then truncated back to the last
 *                                          committed record boundary after a failure
 *   indeterminate_bytes                  : written but the rollback/close could not be
 *                                          proven — may be absent or an exact prefix at
 *                                          the EOF of a retired file, never elsewhere
 *   lost_unwritten_bytes                 : the unwritten remainder of a short write
 *   retention_evicted_bytes              : intentional rotation deletes (not loss)
 *   truncated_records                    : records cut to fit (marked "~T"), kept
 *   torn_tail_files                      : files found with an unterminated tail at open
 *                                          (earlier power cut / card loss) and retired */
typedef struct {
    uint64_t dropped_ring_bytes;
    uint64_t dropped_rotate_blocked_bytes;
    uint64_t dropped_unavailable_bytes;
    uint64_t rolled_back_bytes;
    uint64_t indeterminate_bytes;
    uint64_t lost_unwritten_bytes;
    uint64_t retention_evicted_bytes;
    uint32_t dropped_records;
    uint32_t truncated_records;
    uint32_t torn_tail_files;
    uint32_t write_err, flush_err, fsync_err, close_err, truncate_err, rotate_err, open_err;
    /* the last failure's op/errno live in sd_diag (writer sdlog), retained */
    size_t   buffered_bytes;
    bool     quarantined;    /* current file retired pending rotation */
    bool     rotate_backoff; /* a failed rotation is backing off */
} sd_logger_acct_t;

void sd_logger_acct(sd_logger_acct_t *out);
/* The accounting as one JSON object (heartbeat storage.sdlog). Returns the
 * length, or -1 if it would not fit `cap`. */
int  sd_logger_render_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

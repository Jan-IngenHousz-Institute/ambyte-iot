#pragma once

#include <stdbool.h>
#include <stddef.h>

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

/* Diagnostics snapshot (e.g. for a CLI command). Any out-pointer may be NULL.
 *  active     : the log file is currently open and being written
 *  buffered   : bytes waiting in the RAM ring buffer
 *  dropped    : total bytes dropped on ring overflow since boot
 *  file_bytes : size of the current (active) log file */
void sd_logger_stats(bool *active, size_t *buffered, size_t *dropped, size_t *file_bytes);

#ifdef __cplusplus
}
#endif

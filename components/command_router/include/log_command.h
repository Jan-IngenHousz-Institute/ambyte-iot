#pragma once
/*
 * log_command.h — read-only remote access to the SD log directory
 * (remote diagnostics, 2026-11).
 *
 * WHY: /sdcard/logs/ambyte.log holds the evidence of a field restart (the
 * "boot sync: last reset was …" line, every W/E line since), but only for
 * whoever pulls the card — and nobody copies SD cards by hand. These two
 * commands make it readable over MQTT, bounded and strictly read-only:
 *
 *   {"type":"log_list","id":…}
 *     → {"type":"log_list_result","id","ok","detail","dir",
 *        "files":[{"name","size"}],"omitted"}
 *   {"type":"log_read","id":…,"name":"ambyte.log","offset":…,"length":…}
 *     → {"type":"log_read_result","id","ok","detail","name","size","offset",
 *        "length","eof","encoding":"base64","data"}
 *
 * - `name` is a plain basename that must be FOUND in the log directory:
 *   1–64 chars of [A-Za-z0-9._-], no leading '.', no "..", so no '/', no other
 *   directory, ever.
 * - `offset` ≥ 0 counts from the start; < 0 counts from the end (tail; clamped
 *   to the start of the file). An offset past EOF is refused
 *   ("offset_past_eof": the file was most likely rotated under the reader).
 * - `length` defaults to 4096 and is clamped to 8192; the reply's `length`
 *   is what was actually returned. `data` is standard base64 (RFC 4648, padded):
 *   bounded at 4/3 of the raw bytes (≈ 11 KiB worst case on the wire, far
 *   inside the publish caps), unlike escaped text which can grow 6×.
 *
 * Execution: parsing and validation happen on the esp-mqtt task (cheap, no
 * I/O); the read itself runs on the resident maintenance worker (same pattern
 * as set_timezone) under the maintenance lock and an sdcard_io_begin/end
 * reference. It never waits on the card: absent, parked, I/O-lost or
 * unmount-pending cards and a held maintenance gate are refused at once with
 * an explicit `detail`. Files are opened "rb" — never written, renamed,
 * truncated or deleted (sd_logger's FAT writer is untouched; FATFS_FS_LOCK=0
 * lets a reader open the file the writer holds).
 *
 * Every SD/maintenance probe is injected so tests/log_command_host runs this
 * production file against a real temporary directory.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_READ_DEFAULT_LEN 4096
#define LOG_READ_MAX_LEN     8192
#define LOG_NAME_MAX         64
#define LOG_LIST_MAX_FILES   32

typedef struct {
    void (*reply)(const char *json);
    bool (*submit)(void (*run)(void *), void *arg);
    bool (*maintenance_begin)(void);
    void (*maintenance_end)(void);
    const char *dir;               /* sd_logger_dir() on the target */
    bool (*sd_mounted)(void);
    bool (*sd_parked)(void);
    bool (*sd_io_lost)(void);
    bool (*io_begin)(void);        /* false = unmount pending / card lost */
    void (*io_end)(void);
} log_command_config_t;

void log_command_init(const log_command_config_t *config);
/* True when root is a log_list/log_read command (handled, replied to or
 * queued). False for every other type. Caller still owns root. */
bool log_command_handle(const cJSON *root);

/* Pure helpers, exposed for the host test. */
bool log_command_name_valid(const char *name);
/* Resolve (offset, length) against a file of `size` bytes. NULL on success
 * (start/len set), else the refusal detail. */
const char *log_command_resolve(int64_t size, int64_t offset, int64_t length,
                                int64_t *start, int64_t *len);
/* Standard padded base64; returns the encoded length or 0 when cap is short. */
size_t log_command_base64(const uint8_t *in, size_t n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

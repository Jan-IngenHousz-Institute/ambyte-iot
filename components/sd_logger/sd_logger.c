/*
 * sd_logger.c — tee ESP-IDF logs to rotating text files on the SD card.
 *
 * esp_log_set_vprintf() installs sd_log_vprintf() as the log sink. That hook
 * runs in the context of whatever task logged, so it must be fast and must
 * never block: it tees to the previous (console) sink, formats the line with an
 * RTC wall-clock prefix (ANSI colour stripped), and pushes it into a small
 * lock-protected RAM ring buffer. A separate low-priority task drains the ring
 * to /sdcard/logs/ambyte.log, rotating across SD_LOGGER_MAX_FILES files. SD I/O
 * happens only in that task, so logging itself is decoupled from the (slow,
 * occasionally-absent) card.
 *
 * Write integrity (2026-09 write-corruption audit). Historically the writer
 * ignored every sync and rotation result: a failed fsync was treated as
 * durable, a torn half-line was extended by later appends, and a rotation
 * whose oldest file could not be removed re-ran remove + 5 renames on EVERY
 * write — a FAT metadata storm on a card that was already failing. The rules
 * now are:
 *   - framing is fixed at the PRODUCER: every ring record is one log call,
 *     1..SD_LOGGER_LINE_MAX bytes, ending in exactly one '\n' (F-L0);
 *   - the writer only ever writes whole records, and s_committed is the file
 *     offset after the last successful sync — always a record boundary;
 *   - any failed write/flush/sync rolls the file back to s_committed; if the
 *     rollback itself cannot be proven, the file is QUARANTINED (never appended
 *     to again — the next append waits for a rotation), so a torn tail can only
 *     ever sit at the EOF of a retired file;
 *   - a failed rotation backs off; the current file may grow to twice its cap,
 *     then logging to SD suspends (counted) instead of storming the FAT;
 *   - every byte that does not reach the card intact is attributed to exactly
 *     one accounting bucket (sd_logger_acct), and every fault is recorded in
 *     sd_diag (RTC/NVS — never the SD).
 */

#include "sd_logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "sd_card.h"
#include "sd_diag.h"

/* Verification build only (Sprint 2 H2/H7/H8): the byte-exact logger trace
 * (docs/sdlog-hil-trace.md). SDL_HIL(stmt) compiles to nothing in a release
 * build — the release image carries no trace hook (check_release_no_hil.py). */
#if !defined(EVQ_HOST_FAULTS) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_AMBYTE_EVQ_HIL) && CONFIG_AMBYTE_EVQ_HIL
#include "evq_hil_sdl.h"
#define SDL_HIL(stmt) do { stmt; } while (0)
#else
#define SDL_HIL(stmt) do { } while (0)
#endif

/* Verification build only: route this file's SD operations through the
 * writer-tagged fault wrappers (evq_hil fault io sdlog …). Must stay the LAST
 * include; in a release build the header defines nothing. */
#define EVQ_HIL_WRITER SD_DIAG_W_SDLOG
#include "evq_hil_io_w.h"

/* ── tunables ─────────────────────────────────────────────────────────── */
#define SD_LOGGER_DIR        SD_MOUNT_POINT "/logs"
#define SD_LOGGER_BASENAME   "ambyte"
#ifndef SD_LOGGER_FILE_BYTES
#define SD_LOGGER_FILE_BYTES (1 * 1024 * 1024)   /* per-file cap */
#endif
#define SD_LOGGER_HARD_BYTES (2 * SD_LOGGER_FILE_BYTES)  /* growth ceiling while rotation is blocked */
#define SD_LOGGER_MAX_FILES  6                    /* current + 5 rotated ≈ 6 MB */
#define SD_LOGGER_RING_BYTES (8 * 1024)           /* in-RAM buffer (heap is tight) */
#define SD_LOGGER_LINE_MAX   256                  /* max record, including its '\n' */
#define SD_LOGGER_CHUNK      512                  /* writer pop size (≥ 2 records) */
#define SD_LOGGER_TASK_STACK 4096
#define SD_LOGGER_TASK_PRIO  2                     /* low: below comms/measurement */
#define SD_LOGGER_POLL_MS    250                   /* drain cadence when idle */
#define SD_LOGGER_FSYNC_MS   2000                  /* flush-to-card cadence */
#define SD_LOGGER_ROT_BACKOFF_MS 60000             /* after a failed rotation */
#define SD_LOGGER_FAILLOG_MS 60000                 /* console failure-log rate limit, per class */

_Static_assert(SD_LOGGER_CHUNK >= 2 * SD_LOGGER_LINE_MAX, "a pop must always hold a whole record");

#define TAG "sd_logger"

/* ── lock-protected ring buffer (multi-writer, drop-newest on overflow) ─ */
static uint8_t         s_ring[SD_LOGGER_RING_BYTES];
static volatile size_t s_head;           /* write index */
static volatile size_t s_tail;           /* read index  */
static volatile size_t s_dropped;        /* bytes dropped on overflow */
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

static vprintf_like_t  s_prev_vprintf;   /* console sink (tee target) */
static volatile bool   s_file_open;      /* stats: file currently open */
static volatile size_t s_file_bytes;     /* current file size as written */
static size_t          s_committed;      /* file offset after the last good sync (record boundary) */
static bool            s_quarantined;    /* current file must not be appended to: rotate first */
static FILE           *s_fp;
static bool            s_started;
static TaskHandle_t    s_writer;         /* recursion guard: its own logs stay console-only */
static sd_logger_acct_t s_acct;          /* written by the writer task (and ring_push under s_mux) */

/* Pre-reboot handshake: prepare_shutdown() sets s_stop_req; the writer task (the
 * sole owner of s_fp) drains the ring, flushes, closes, and sets s_stopped, then
 * parks. Keeping all file I/O in the writer task avoids a cross-task race on s_fp. */
static volatile bool   s_stop_req;
static volatile bool   s_stopped;

/* Low-battery park handshake: pause() asks the writer to drain + close the file
 * and then HOLD (keep buffering to the RAM ring, touch no FATFS) until resume().
 * Unlike the card-loss path this is a clean close — the card is still healthy, so
 * nothing is abandoned/leaked, and a park can safely happen every night. Unlike
 * prepare_shutdown() the writer does not park forever, so the logger survives the
 * park/unpark cycle. Both flags are owned by the writer task except the request
 * bit itself. */
static volatile bool   s_pause_req;
static volatile bool   s_paused;

/* Rotation backoff: while now < s_rot_retry_at, no rotation is attempted. */
static bool            s_rot_backoff;
static TickType_t      s_rot_retry_at;

static inline size_t ring_used(void)
{
    return (s_head + SD_LOGGER_RING_BYTES - s_tail) % SD_LOGGER_RING_BYTES;
}

/* Push one whole record atomically; drop it whole if it doesn't fit, so a record
 * is never split. Safe from any task — only a brief memcpy under lock. */
static void ring_push(const uint8_t *p, size_t n)
{
    if (n == 0 || n >= SD_LOGGER_RING_BYTES) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t used  = (s_head + SD_LOGGER_RING_BYTES - s_tail) % SD_LOGGER_RING_BYTES;
    size_t freeb = SD_LOGGER_RING_BYTES - 1 - used;
    if (n <= freeb) {
        size_t first = SD_LOGGER_RING_BYTES - s_head;
        if (first > n) first = n;
        memcpy(&s_ring[s_head], p, first);
        if (n > first) memcpy(&s_ring[0], p + first, n - first);
        s_head = (s_head + n) % SD_LOGGER_RING_BYTES;
        SDL_HIL(hil_sdl_push(p, n, true));       /* inside the ring lock: trace order == ring order */
    } else {
        s_dropped += n;
        s_acct.dropped_ring_bytes += n;
        s_acct.dropped_records++;
        SDL_HIL(hil_sdl_push(p, n, false));
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    SDL_HIL(hil_sdl_after_push());
}

/* Pop the longest run of WHOLE records (ending in '\n') that fits `cap`.
 * Producer framing guarantees a non-empty ring always yields ≥ 1 record. */
static size_t ring_pop(uint8_t *out, size_t cap)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t used = (s_head + SD_LOGGER_RING_BYTES - s_tail) % SD_LOGGER_RING_BYTES;
    size_t lim  = used < cap ? used : cap;
    size_t n = 0;
    for (size_t i = 0; i < lim; i++) {
        if (s_ring[(s_tail + i) % SD_LOGGER_RING_BYTES] == '\n') n = i + 1;
    }
    size_t first = SD_LOGGER_RING_BYTES - s_tail;
    if (first > n) first = n;
    memcpy(out, &s_ring[s_tail], first);
    if (n > first) memcpy(out + first, &s_ring[0], n - first);
    s_tail = (s_tail + n) % SD_LOGGER_RING_BYTES;
    SDL_HIL(hil_sdl_pop(n));
    portEXIT_CRITICAL_SAFE(&s_mux);
    return n;
}

/* ── log sink ─────────────────────────────────────────────────────────── */

/* The IDF log line begins with an optional ANSI colour escape, then the level
 * letter (E/W/I/D/V). The SD file keeps only WARN and ERROR — INFO/DEBUG are far
 * too high-volume to write continuously to a consumer SD card sharing the FAT
 * with the events DB (that combination corrupted the DB). */
static bool sd_log_level_kept(const char *fmt)
{
    const char *p = fmt;
    if (*p == '\033') {                 /* skip CSI colour escape: ESC [ … final @-~ */
        p++;
        if (*p == '[') { p++; while (*p && !(*p >= '@' && *p <= '~')) p++; if (*p) p++; }
    }
    return (*p == 'E' || *p == 'W');
}

static int sd_log_vprintf(const char *fmt, va_list ap)
{
    /* 1) Console tee. vprintf consumes the va_list, so hand it a private copy. */
    va_list ap_console;
    va_copy(ap_console, ap);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, ap_console) : 0;
    va_end(ap_console);

    /* The formatting below isn't ISR-safe (localtime_r/vsnprintf take locks),
     * and normal ESP_LOGx is never called from an ISR — guard just in case. */
    if (xPortInIsrContext()) return ret;

    /* 1b) SD file is WARN/ERROR-only; the console (above) still sees everything. */
    if (!sd_log_level_kept(fmt)) return ret;

    /* 1c) No recursion: the writer's own failure reports describe the card it
     * cannot write — teeing them back onto that card re-arms the failing I/O.
     * They stay on the console (and in sd_diag). */
    if (s_writer != NULL && xTaskGetCurrentTaskHandle() == s_writer) return ret;

    /* 2) Build "<RTC wall-clock>  <message>" in one buffer. System time was set
     * from the PCF2131 at boot, so time() is cheap (no I2C); before the RTC
     * syncs it reads ~1970, which is acceptable. */
    char out[SD_LOGGER_LINE_MAX + 1];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    size_t off = strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S  ", &tmv);   /* 21 chars */

    int m = vsnprintf(out + off, sizeof out - off, fmt, ap);
    if (m < 0) return ret;
    bool trunc = (size_t)m >= sizeof out - off;
    size_t end = off + (trunc ? sizeof out - off - 1 : (size_t)m);

    /* Strip ANSI colour escapes (ESC '[' … final @-~) in place; IDF embeds them
     * in the log format string when CONFIG_LOG_COLORS is on. */
    size_t w = off;
    for (size_t r = off; r < end; r++) {
        char c = out[r];
        if (c == '\033') {
            r++;
            if (r < end && out[r] == '[') {
                r++;
                while (r < end && !(out[r] >= '@' && out[r] <= '~')) r++;
            }
            continue;   /* the for-loop r++ also skips the final byte */
        }
        out[w++] = c;
    }

    /* 3) F-L0 framing: one log call = one record = exactly one terminal '\n'.
     * A missing newline (or a cut) would otherwise merge this record with the
     * next one in the file, and a bare '\n' mid-message would split it. */
    while (w > off && (out[w - 1] == '\n' || out[w - 1] == '\r')) w--;
    for (size_t i = off; i < w; i++) {
        if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    }
    if (w > SD_LOGGER_LINE_MAX - 1) { w = SD_LOGGER_LINE_MAX - 1; trunc = true; }
    if (trunc) {
        if (w > SD_LOGGER_LINE_MAX - 3) w = SD_LOGGER_LINE_MAX - 3;
        out[w++] = '~';
        out[w++] = 'T';                 /* visible "this record was cut" marker */
        portENTER_CRITICAL_SAFE(&s_mux);
        s_acct.truncated_records++;
        portEXIT_CRITICAL_SAFE(&s_mux);
    }
    out[w++] = '\n';
    ring_push((const uint8_t *)out, w);
    return ret;
}

/* ── failure reporting (console-only, rate-limited; sd_diag always) ───── */

typedef enum { FL_WRITE, FL_SYNC, FL_ROLLBACK, FL_ROTATE, FL_OPEN, FL_CLOSE, FL_N } faillog_class_t;
static TickType_t s_faillog_at[FL_N];
static bool       s_faillog_any[FL_N];

static void note_fault(faillog_class_t cls, sd_diag_op_t op, int err)
{
    sd_diag_fault(SD_DIAG_W_SDLOG, op, err);
    TickType_t now = xTaskGetTickCount();
    if (!s_faillog_any[cls] || (now - s_faillog_at[cls]) >= pdMS_TO_TICKS(SD_LOGGER_FAILLOG_MS)) {
        s_faillog_any[cls] = true;
        s_faillog_at[cls] = now;
        ESP_LOGW(TAG, "%s failed errno=%d", sd_diag_op_name(op), err);
    }
}

/* ── file handling + rotation ─────────────────────────────────────────── */

static void log_path(char *buf, size_t cap, int idx)
{
    if (idx == 0) snprintf(buf, cap, "%s/%s.log",    SD_LOGGER_DIR, SD_LOGGER_BASENAME);
    else          snprintf(buf, cap, "%s/%s.%d.log", SD_LOGGER_DIR, SD_LOGGER_BASENAME, idx);
}

/* ambyte.log → ambyte.1.log → … → ambyte.(MAX-1).log; the oldest is deleted.
 * FAT rename never replaces an existing target (EEXIST), so a step that fails
 * leaves every file intact; ENOENT (a not-yet-created file) is harmless.
 * Returns false on the first real failure — the caller backs off instead of
 * retrying on every write. */
static bool rotate_files(void)
{
    char a[SD_CARD_PATH_MAX], b[SD_CARD_PATH_MAX];
    struct stat st;
    log_path(a, sizeof a, SD_LOGGER_MAX_FILES - 1);
    long evicted = stat(a, &st) == 0 ? (long)st.st_size : -1;
    if (remove(a) != 0 && errno != ENOENT) {
        SDL_HIL(hil_sdl_ev(HIL_SDL_ROT, HIL_SDL_ROT_FAIL_REMOVE, (uint32_t)errno, 0, NULL));
        note_fault(FL_ROTATE, SD_DIAG_OP_REMOVE, errno);
        s_acct.rotate_err++;
        return false;
    }
    if (evicted > 0) s_acct.retention_evicted_bytes += (uint64_t)evicted;   /* intentional, not loss */
    for (int i = SD_LOGGER_MAX_FILES - 2; i >= 0; i--) {
        log_path(a, sizeof a, i);
        log_path(b, sizeof b, i + 1);
        if (rename(a, b) != 0 && errno != ENOENT) {
            SDL_HIL(hil_sdl_ev(HIL_SDL_ROT, HIL_SDL_ROT_FAIL_RENAME, (uint32_t)errno, (uint32_t)i, NULL));
            note_fault(FL_ROTATE, SD_DIAG_OP_RENAME, errno);
            s_acct.rotate_err++;
            return false;
        }
    }
    SDL_HIL(hil_sdl_ev(HIL_SDL_ROT, HIL_SDL_ROT_OK, 0, 0, NULL));
    return true;
}

static bool open_log(void)
{
    mkdir(SD_LOGGER_DIR, 0777);   /* EEXIST is the normal case; a real failure surfaces at fopen */
    char path[SD_CARD_PATH_MAX];
    log_path(path, sizeof path, 0);
    /* "a+": appends always land at EOF (vfs_fat honours O_APPEND per write), and
     * the last byte can be read back to check the tail. Unbuffered: a failed
     * write must not linger in a stdio buffer that a later flush/close would
     * write back past a rollback point. */
    FILE *f = fopen(path, "a+");
    if (f == NULL) {
        note_fault(FL_OPEN, SD_DIAG_OP_OPEN, errno);
        s_acct.open_err++;
        return false;
    }
    setvbuf(f, NULL, _IONBF, 0);
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz < 0) {
        /* Never assume 0: appending at a guessed offset breaks the size cap
         * and the committed-offset bookkeeping. */
        note_fault(FL_OPEN, SD_DIAG_OP_OPEN, errno ? errno : EIO);
        s_acct.open_err++;
        fclose(f);
        return false;
    }
    bool torn = false;
    if (sz > 0) {
        /* A tail without '\n' is a record torn by a power cut or a lost card
         * (earlier boot or earlier mount): appending would merge the next
         * record into it. Retire the file by rotation instead. */
        int c = EOF;
        if (fseek(f, -1, SEEK_END) == 0) c = fgetc(f);
        if (c != '\n') torn = true;
    }
    s_fp = f;
    s_file_bytes = (size_t)sz;
    s_committed = (size_t)sz;
    s_file_open = true;
    if (torn) {
        s_quarantined = true;
        s_acct.torn_tail_files++;
    }
    SDL_HIL(hil_sdl_ev(HIL_SDL_OPEN, (uint32_t)sz, torn ? 1u : 0u, 0, SD_LOGGER_BASENAME ".log"));
    return true;
}

/* Close after a successful commit. Unbuffered + committed ⇒ nothing is lost if
 * fclose reports an error, but the error is still attributed. */
static void close_log(void)
{
    if (s_fp) {
        if (fclose(s_fp) != 0) {
            SDL_HIL(hil_sdl_ev(HIL_SDL_CLOSE, HIL_SDL_CLOSE_ERR, 0, 0, NULL));
            note_fault(FL_CLOSE, SD_DIAG_OP_CLOSE, errno);
            s_acct.close_err++;
            if (s_file_bytes > s_committed) s_acct.indeterminate_bytes += s_file_bytes - s_committed;
        } else {
            SDL_HIL(hil_sdl_ev(HIL_SDL_CLOSE, HIL_SDL_CLOSE_OK, 0, 0, NULL));
        }
        s_fp = NULL;
    }
    s_file_open = false;
}

/* Abandon without touching FATFS (card lost / gate refused): fclose on a volume
 * the monitor may free is a UAF (audit R-8). Whatever was written since the last
 * sync may or may not be on the card. */
static void abandon_log(void)
{
    if (s_fp != NULL) SDL_HIL(hil_sdl_ev(HIL_SDL_CLOSE, HIL_SDL_CLOSE_ABANDON, 0, 0, NULL));
    if (s_fp != NULL && s_file_bytes > s_committed) s_acct.indeterminate_bytes += s_file_bytes - s_committed;
    s_fp = NULL;
    s_file_open = false;
}

/* Roll the open file back to s_committed after a failed write/flush/sync
 * (F-L2). `extra` = bytes of a partially written chunk (already on the card
 * beyond s_file_bytes). On proven rollback the handle stays usable; otherwise
 * the file is quarantined and closed. */
static void rollback(size_t extra)
{
    size_t since = s_file_bytes - s_committed + extra;
    int fd = fileno(s_fp);
    if (ftruncate(fd, (off_t)s_committed) != 0) {
        SDL_HIL(hil_sdl_ev(HIL_SDL_RB, (uint32_t)s_committed, (uint32_t)since, HIL_SDL_RB_QUAR_TRUNC, NULL));
        note_fault(FL_ROLLBACK, SD_DIAG_OP_TRUNCATE, errno);
        s_acct.truncate_err++;
    } else if (fsync(fd) != 0) {
        SDL_HIL(hil_sdl_ev(HIL_SDL_RB, (uint32_t)s_committed, (uint32_t)since, HIL_SDL_RB_QUAR_FSYNC, NULL));
        note_fault(FL_ROLLBACK, SD_DIAG_OP_FSYNC, errno);
        s_acct.fsync_err++;
    } else {
        SDL_HIL(hil_sdl_ev(HIL_SDL_RB, (uint32_t)s_committed, (uint32_t)since, HIL_SDL_RB_OK, NULL));
        s_acct.rolled_back_bytes += since;
        s_file_bytes = s_committed;
        return;
    }
    /* Rollback not proven: the bytes since commit may or may not be at EOF. The
     * file is never appended to again; any tail stays at ITS end. */
    s_acct.indeterminate_bytes += since;
    s_file_bytes = s_committed;          /* nothing more is attributed to this handle */
    s_quarantined = true;
    if (fclose(s_fp) != 0) s_acct.close_err++;
    s_fp = NULL;
    s_file_open = false;
}

/* Commit everything written so far (fflush + fsync). False → rolled back. */
static bool commit(void)
{
    if (s_fp == NULL || s_file_bytes == s_committed) return true;
    if (fflush(s_fp) != 0) {
        note_fault(FL_SYNC, SD_DIAG_OP_FLUSH, errno);
        s_acct.flush_err++;
    } else if (fsync(fileno(s_fp)) != 0) {
        note_fault(FL_SYNC, SD_DIAG_OP_FSYNC, errno);
        s_acct.fsync_err++;
    } else {
        s_committed = s_file_bytes;
        SDL_HIL(hil_sdl_ev(HIL_SDL_COMMIT, (uint32_t)s_committed, 0, 0, NULL));
        return true;
    }
    sdcard_report_io_error();
    rollback(0);
    return false;
}

/* Write one chunk of whole records. False → rolled back / quarantined. */
static bool write_chunk(const uint8_t *buf, size_t n)
{
    errno = 0;
    size_t w = fwrite(buf, 1, n, s_fp);
    SDL_HIL(hil_sdl_ev(HIL_SDL_WR, (uint32_t)n, (uint32_t)w, (uint32_t)s_file_bytes, NULL));
    if (w == n) {
        s_file_bytes += n;
        return true;
    }
    note_fault(FL_WRITE, SD_DIAG_OP_WRITE, errno ? errno : EIO);
    s_acct.write_err++;
    s_acct.lost_unwritten_bytes += n - w;
    sdcard_report_io_error();
    rollback(w);
    return false;
}

typedef enum { ROOM_OK, ROOM_ROTATE_BLOCKED, ROOM_NO_FILE } room_t;

/* A popped chunk that cannot be written is attributed, never silently lost. */
static void drop_chunk(room_t why, size_t n)
{
    SDL_HIL(hil_sdl_ev(HIL_SDL_DROP, why == ROOM_ROTATE_BLOCKED ? HIL_SDL_DROP_ROTATE_BLOCKED : HIL_SDL_DROP_UNAVAILABLE,
                       (uint32_t)n, 0, NULL));
    if (why == ROOM_ROTATE_BLOCKED) s_acct.dropped_rotate_blocked_bytes += n;
    else s_acct.dropped_unavailable_bytes += n;
}

/* Make the open file appendable for `n` more bytes, rotating when due. Caller
 * holds an io ref. */
static room_t ensure_room(size_t n)
{
    if (s_fp == NULL && !open_log()) return ROOM_NO_FILE;
    bool due = s_quarantined || s_file_bytes + n > SD_LOGGER_FILE_BYTES;
    if (!due) return ROOM_OK;
    TickType_t now = xTaskGetTickCount();
    if (s_rot_backoff && (int32_t)(now - s_rot_retry_at) < 0) {
        /* Backing off: a healthy (non-quarantined) file may keep growing to the
         * hard ceiling; past it, or if quarantined, drop instead of retrying. */
        return (!s_quarantined && s_file_bytes + n <= SD_LOGGER_HARD_BYTES) ? ROOM_OK : ROOM_ROTATE_BLOCKED;
    }
    (void)commit();                       /* nothing uncommitted crosses a rotation (failure → rolled back) */
    if (s_fp != NULL) close_log();
    bool ok = rotate_files();
    if (ok) {
        s_rot_backoff = false;
        s_quarantined = false;
    } else {
        s_rot_backoff = true;
        s_rot_retry_at = now + pdMS_TO_TICKS(SD_LOGGER_ROT_BACKOFF_MS);
    }
    if (!open_log()) return ROOM_NO_FILE;
    if (s_quarantined) return ROOM_ROTATE_BLOCKED;   /* rotation failed, or the new file is torn */
    if (s_file_bytes + n <= (ok ? SD_LOGGER_FILE_BYTES : SD_LOGGER_HARD_BYTES)) return ROOM_OK;
    return ROOM_ROTATE_BLOCKED;
}

/* Drain the ring to the file (pause/stop): whole records only, same failure
 * rules as normal service. Caller holds an io ref. */
static void drain_and_close(uint8_t *buf, size_t cap)
{
    size_t n;
    while ((n = ring_pop(buf, cap)) > 0) {
        room_t r = ensure_room(n);
        if (r != ROOM_OK) { drop_chunk(r, n); continue; }
        (void)write_chunk(buf, n);
    }
    (void)commit();
    close_log();
}

static void writer_task(void *arg)
{
    (void)arg;
    uint8_t buf[SD_LOGGER_CHUNK];
    TickType_t last_fsync = xTaskGetTickCount();

    for (;;) {
        /* Pre-reboot drain (Item B): flush whatever is buffered to the current
         * file, fsync + close it so sdcard_unmount() can finalize FATFS cleanly,
         * then park until the reboot. Only the writer task ever touches s_fp, so
         * this is race-free. Skipped if the card is already gone. */
        if (s_stop_req) {
            if (!sdcard_io_lost() && sdcard_is_mounted() && sdcard_io_begin()) {
                drain_and_close(buf, sizeof buf);
                sdcard_io_end();
            } else {
                abandon_log();
            }
            s_stopped = true;
            vTaskDelay(portMAX_DELAY);   /* parked — the reboot follows shortly */
        }

        /* Low-battery park: drain + fsync + close ONCE, then hold with no FATFS
         * traffic until resumed (the ring keeps buffering; overflow drops newest).
         * The close is io_begin-gated like every other FATFS touch; if the gate
         * refuses (card lost / teardown pending mid-park) the handle is abandoned
         * exactly like the loss branch below — fclose on a freed volume is a UAF. */
        if (s_pause_req) {
            if (!s_paused) {
                if (!sdcard_io_lost() && sdcard_is_mounted() && sdcard_io_begin()) {
                    drain_and_close(buf, sizeof buf);
                    sdcard_io_end();
                } else {
                    abandon_log();
                }
                s_paused = true;
            }
            vTaskDelay(pdMS_TO_TICKS(SD_LOGGER_POLL_MS * 2));
            continue;
        }
        if (s_paused) s_paused = false;   /* resumed — fall through to normal service */

        /* Check the lock-free loss latch BEFORE sdcard_is_mounted(): when a card is
         * pulled this is the fast loop that re-arms the failing I/O, so it must stop
         * the instant loss is latched. Do NOT close here — abandon (see abandon_log);
         * the leaked handle is one-per-loss and bounded, and the ring keeps buffering. */
        if (sdcard_io_lost() || !sdcard_is_mounted()) {
            abandon_log();
            vTaskDelay(pdMS_TO_TICKS(SD_LOGGER_POLL_MS * 2));
            continue;
        }

        /* Gate the whole FATFS-touching iteration so an unmount can't free the volume
         * mid-op (audit F1/R-8). EVERY exit below routes through the single iter_end. */
        if (!sdcard_io_begin()) {
            vTaskDelay(pdMS_TO_TICKS(SD_LOGGER_POLL_MS));
            continue;
        }
        if (s_fp == NULL && !open_log()) {
            sdcard_report_io_error();                      /* open on a gone card → loss */
            sdcard_io_end();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        {
            size_t n = ring_pop(buf, sizeof buf);
            bool idle = n == 0;
            if (n > 0) {
                room_t r = ensure_room(n);
                if (r != ROOM_OK) {
                    drop_chunk(r, n);
                } else if (write_chunk(buf, n)) {
                    sdcard_report_io_ok();                 /* good write → reset streak */
                }
            }

            /* Sync only when there's something uncommitted — with WARN/ERROR-only
             * capture the ring is usually empty, so the card sees no periodic
             * writes (an idle fsync still touches the FAT). */
            if (s_fp && s_file_bytes > s_committed &&
                (xTaskGetTickCount() - last_fsync) >= pdMS_TO_TICKS(SD_LOGGER_FSYNC_MS)) {
                (void)commit();
                last_fsync = xTaskGetTickCount();
            }
            sdcard_io_end();
            if (idle) vTaskDelay(pdMS_TO_TICKS(SD_LOGGER_POLL_MS));   /* never delay holding a ref */
        }
    }
}

/* ── public API ───────────────────────────────────────────────────────── */

esp_err_t sd_logger_init(void)
{
    if (s_started) return ESP_OK;
    SDL_HIL(hil_sdl_boot_hook());           /* auto-armed trace starts before the writer's first OPEN */

    s_head = s_tail = 0;
    s_dropped = 0;

    s_prev_vprintf = esp_log_set_vprintf(sd_log_vprintf);
    if (xTaskCreate(writer_task, "sd_logger", SD_LOGGER_TASK_STACK,
                    NULL, SD_LOGGER_TASK_PRIO, &s_writer) != pdPASS) {
        esp_log_set_vprintf(s_prev_vprintf);   /* roll back the hook */
        s_prev_vprintf = NULL;
        s_writer = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    ESP_LOGI(TAG, "SD logging started (WARN/ERROR only) -> %s/%s.log (%d files x %d KiB)",
             SD_LOGGER_DIR, SD_LOGGER_BASENAME,
             SD_LOGGER_MAX_FILES, SD_LOGGER_FILE_BYTES / 1024);
    return ESP_OK;
}

void sd_logger_pause(void)
{
    if (!s_started) return;
    s_pause_req = true;
    /* Bounded wait for the writer to drain + close (it polls at SD_LOGGER_POLL_MS),
     * so the caller can unmount immediately after; if the writer is stuck in a slow
     * failing op the caller's unmount drain covers the remainder. */
    for (int i = 0; i < 100 && !s_paused; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void sd_logger_resume(void)
{
    if (!s_started) return;
    s_pause_req = false;   /* writer clears s_paused and reopens on its next cycle */
}

void sd_logger_prepare_shutdown(void)
{
    if (!s_started) return;
    s_stop_req = true;
    /* Wait (bounded ~1 s) for the writer task to flush + close. The writer polls
     * at SD_LOGGER_POLL_MS (idle) / *2 (card out), so it observes the flag well
     * within this window; a reboot must not hang if it somehow doesn't. */
    for (int i = 0; i < 100 && !s_stopped; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void sd_logger_stats(bool *active, size_t *buffered, size_t *dropped, size_t *file_bytes)
{
    if (active)     *active     = s_file_open;
    if (buffered)   *buffered   = ring_used();   /* lock-free read: approximate */
    if (dropped)    *dropped    = s_dropped;
    if (file_bytes) *file_bytes = s_file_bytes;
}

void sd_logger_acct(sd_logger_acct_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    *out = s_acct;
    out->buffered_bytes = ring_used();
    out->quarantined = s_quarantined;
    out->rotate_backoff = s_rot_backoff;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

int sd_logger_render_json(char *buf, size_t cap)
{
    /* Compact keys (heartbeat budget): quar=quarantined, backoff=rotation
     * backoff, rb=rolled_back, indet=indeterminate, unwr=lost_unwritten,
     * ring/rot/unav=dropped (ring full / rotation blocked / no file),
     * evict=retention_evicted, trunc=truncated_records, torn=torn_tail_files;
     * byte counts except trunc/torn/err. */
    sd_logger_acct_t a;
    sd_logger_acct(&a);
    int n = snprintf(buf, cap,
                     "{\"quar\":%d,\"backoff\":%d,\"rb\":%llu,\"indet\":%llu,\"unwr\":%llu,\"ring\":%llu,"
                     "\"rot\":%llu,\"unav\":%llu,\"evict\":%llu,\"trunc\":%u,\"torn\":%u,\"err\":%u}",
                     a.quarantined, a.rotate_backoff,
                     (unsigned long long)a.rolled_back_bytes, (unsigned long long)a.indeterminate_bytes,
                     (unsigned long long)a.lost_unwritten_bytes, (unsigned long long)a.dropped_ring_bytes,
                     (unsigned long long)a.dropped_rotate_blocked_bytes, (unsigned long long)a.dropped_unavailable_bytes,
                     (unsigned long long)a.retention_evicted_bytes, (unsigned)a.truncated_records,
                     (unsigned)a.torn_tail_files,
                     (unsigned)(a.write_err + a.flush_err + a.fsync_err + a.close_err + a.truncate_err +
                                a.rotate_err + a.open_err));
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

#if defined(CONFIG_AMBYTE_EVQ_HIL) && CONFIG_AMBYTE_EVQ_HIL
/* H7 quiesce support (verification build only): the pause handshake is
 * complete only when the writer itself reports it is holding. */
bool sd_logger_hil_paused(void) { return s_paused; }

void sd_logger_hil_state(uint32_t *file_bytes, uint32_t *committed, bool *quar, uint32_t *backoff_ms_left, bool *open)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    *file_bytes = (uint32_t)s_file_bytes;
    *committed = (uint32_t)s_committed;
    *quar = s_quarantined;
    *open = s_file_open;
    TickType_t now = xTaskGetTickCount();
#ifdef portTICK_PERIOD_MS
    const uint32_t ms_per_tick = portTICK_PERIOD_MS;
#else
    const uint32_t ms_per_tick = 1;                 /* host stand-in: pdMS_TO_TICKS(ms) == ms */
#endif
    *backoff_ms_left = (s_rot_backoff && (int32_t)(s_rot_retry_at - now) > 0)
                           ? (uint32_t)(s_rot_retry_at - now) * ms_per_tick : 0;
    portEXIT_CRITICAL_SAFE(&s_mux);
}
#endif

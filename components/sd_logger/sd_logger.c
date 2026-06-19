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
 */

#include "sd_logger.h"

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

/* ── tunables ─────────────────────────────────────────────────────────── */
#define SD_LOGGER_DIR        SD_MOUNT_POINT "/logs"
#define SD_LOGGER_BASENAME   "ambyte"
#define SD_LOGGER_FILE_BYTES (1 * 1024 * 1024)   /* per-file cap */
#define SD_LOGGER_MAX_FILES  6                    /* current + 5 rotated ≈ 6 MB */
#define SD_LOGGER_RING_BYTES (8 * 1024)           /* in-RAM buffer (heap is tight) */
#define SD_LOGGER_LINE_MAX   256                  /* max formatted line */
#define SD_LOGGER_TASK_STACK 4096
#define SD_LOGGER_TASK_PRIO  2                     /* low: below comms/measurement */
#define SD_LOGGER_POLL_MS    250                   /* drain cadence when idle */
#define SD_LOGGER_FSYNC_MS   2000                  /* flush-to-card cadence */

#define TAG "sd_logger"

/* ── lock-protected ring buffer (multi-writer, drop-newest on overflow) ─ */
static uint8_t         s_ring[SD_LOGGER_RING_BYTES];
static volatile size_t s_head;           /* write index */
static volatile size_t s_tail;           /* read index  */
static volatile size_t s_dropped;        /* bytes dropped on overflow */
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

static vprintf_like_t  s_prev_vprintf;   /* console sink (tee target) */
static volatile bool   s_file_open;      /* stats: file currently open */
static volatile size_t s_file_bytes;     /* stats: current file size */
static FILE           *s_fp;
static bool            s_started;

static inline size_t ring_used(void)
{
    return (s_head + SD_LOGGER_RING_BYTES - s_tail) % SD_LOGGER_RING_BYTES;
}

/* Push n bytes atomically; drop the whole chunk if it doesn't fit so lines are
 * never split. Safe from any task (and ISR) — only a brief memcpy under lock. */
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
    } else {
        s_dropped += n;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

/* Pop up to cap bytes into out; returns the count copied. */
static size_t ring_pop(uint8_t *out, size_t cap)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t used  = (s_head + SD_LOGGER_RING_BYTES - s_tail) % SD_LOGGER_RING_BYTES;
    size_t n     = used < cap ? used : cap;
    size_t first = SD_LOGGER_RING_BYTES - s_tail;
    if (first > n) first = n;
    memcpy(out, &s_ring[s_tail], first);
    if (n > first) memcpy(out + first, &s_ring[0], n - first);
    s_tail = (s_tail + n) % SD_LOGGER_RING_BYTES;
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

    /* 2) Build "<RTC wall-clock>  <message>" in one buffer. System time was set
     * from the PCF2131 at boot, so time() is cheap (no I2C); before the RTC
     * syncs it reads ~1970, which is acceptable. */
    char out[SD_LOGGER_LINE_MAX];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    size_t off = strftime(out, sizeof out, "%Y-%m-%d %H:%M:%S  ", &tmv);   /* 21 chars */

    int m = vsnprintf(out + off, sizeof out - off, fmt, ap);
    if (m < 0) return ret;
    size_t end = off + ((size_t)m < sizeof out - off ? (size_t)m : sizeof out - off - 1);

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
    ring_push((const uint8_t *)out, w);
    return ret;
}

/* ── file handling + rotation ─────────────────────────────────────────── */

static void log_path(char *buf, size_t cap, int idx)
{
    if (idx == 0) snprintf(buf, cap, "%s/%s.log",    SD_LOGGER_DIR, SD_LOGGER_BASENAME);
    else          snprintf(buf, cap, "%s/%s.%d.log", SD_LOGGER_DIR, SD_LOGGER_BASENAME, idx);
}

/* ambyte.log → ambyte.1.log → … → ambyte.(MAX-1).log; the oldest is deleted. */
static void rotate_files(void)
{
    char a[SD_CARD_PATH_MAX], b[SD_CARD_PATH_MAX];
    log_path(a, sizeof a, SD_LOGGER_MAX_FILES - 1);
    remove(a);
    for (int i = SD_LOGGER_MAX_FILES - 2; i >= 0; i--) {
        log_path(a, sizeof a, i);
        log_path(b, sizeof b, i + 1);
        rename(a, b);   /* ENOENT on a not-yet-created file is harmless */
    }
}

static bool open_log(void)
{
    mkdir(SD_LOGGER_DIR, 0777);   /* ignore EEXIST */
    char path[SD_CARD_PATH_MAX];
    log_path(path, sizeof path, 0);
    s_fp = fopen(path, "a");
    if (s_fp == NULL) return false;
    fseek(s_fp, 0, SEEK_END);
    long sz = ftell(s_fp);
    s_file_bytes = sz > 0 ? (size_t)sz : 0;
    s_file_open = true;
    return true;
}

static void close_log(void)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    s_file_open = false;
}

static void writer_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    TickType_t last_fsync = xTaskGetTickCount();
    bool dirty = false;                                    /* unflushed bytes pending */

    for (;;) {
        /* Check the lock-free loss latch BEFORE sdcard_is_mounted() (which takes
         * the contended sd_card lock): when a card is pulled this is the fast
         * loop that re-arms the failing I/O, so it must stop the instant loss is
         * latched and not keep poking the gone card. */
        if (sdcard_io_lost() || !sdcard_is_mounted()) {
            close_log();                                   /* ring keeps buffering */
            dirty = false;
            vTaskDelay(pdMS_TO_TICKS(SD_LOGGER_POLL_MS * 2));
            continue;
        }
        if (s_fp == NULL && !open_log()) {
            sdcard_report_io_error();                      /* open on a gone card → loss */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t n = ring_pop(buf, sizeof buf);
        if (n > 0) {
            if (s_file_bytes + n > SD_LOGGER_FILE_BYTES) {
                close_log();
                rotate_files();
                if (!open_log()) { sdcard_report_io_error(); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            }
            if (fwrite(buf, 1, n, s_fp) != n) {            /* card pulled / full */
                close_log();
                sdcard_report_io_error();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            sdcard_report_io_ok();                         /* good write → reset streak */
            s_file_bytes += n;
            dirty = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(SD_LOGGER_POLL_MS));  /* idle */
        }

        /* Flush only when there's something to flush — with WARN/ERROR-only
         * capture the ring is usually empty, so the card sees no periodic writes
         * (an idle fsync still touches the FAT). */
        if (s_fp && dirty && (xTaskGetTickCount() - last_fsync) >= pdMS_TO_TICKS(SD_LOGGER_FSYNC_MS)) {
            fflush(s_fp);
            fsync(fileno(s_fp));
            last_fsync = xTaskGetTickCount();
            dirty = false;
        }
    }
}

/* ── public API ───────────────────────────────────────────────────────── */

esp_err_t sd_logger_init(void)
{
    if (s_started) return ESP_OK;

    s_head = s_tail = 0;
    s_dropped = 0;

    s_prev_vprintf = esp_log_set_vprintf(sd_log_vprintf);
    if (xTaskCreate(writer_task, "sd_logger", SD_LOGGER_TASK_STACK,
                    NULL, SD_LOGGER_TASK_PRIO, NULL) != pdPASS) {
        esp_log_set_vprintf(s_prev_vprintf);   /* roll back the hook */
        s_prev_vprintf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    ESP_LOGI(TAG, "SD logging started (WARN/ERROR only) -> %s/%s.log (%d files x %d KiB)",
             SD_LOGGER_DIR, SD_LOGGER_BASENAME,
             SD_LOGGER_MAX_FILES, SD_LOGGER_FILE_BYTES / 1024);
    return ESP_OK;
}

void sd_logger_stats(bool *active, size_t *buffered, size_t *dropped, size_t *file_bytes)
{
    if (active)     *active     = s_file_open;
    if (buffered)   *buffered   = ring_used();   /* lock-free read: approximate */
    if (dropped)    *dropped    = s_dropped;
    if (file_bytes) *file_bytes = s_file_bytes;
}

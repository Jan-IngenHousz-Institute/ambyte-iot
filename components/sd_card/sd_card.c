#include "sd_card.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define TAG "sd_card"
#define SD_CARD_ACCESS_TIMEOUT_TICKS pdMS_TO_TICKS(1000)

/* Forward decls (definitions live in the hot-plug/latch section below). */
static void sdcard_io_reset(void);
static void sdcard_note_corrupt_mount(void);
/* Drain in-flight FATFS ops then unmount; NEVER frees the volume while an op is in
 * flight (returns false = deferred, caller/monitor retries). Caller holds the mutex. */
static bool sdcard_drain_and_unmount_locked(const char *reason);

typedef struct {
    gpio_num_t clk;
    gpio_num_t cmd;
    gpio_num_t d0;
    gpio_num_t d1;
    gpio_num_t d2;
    gpio_num_t d3;
    int width;
} sdcard_pin_config_t;

typedef struct {
    SemaphoreHandle_t mutex;
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sdmmc_card_t *card;
    bool initialized;
    volatile bool mounted;   /* read lock-free by sdcard_is_mounted (advisory); real FS
                              * access is authorized by sdcard_io_begin, not this flag */
} sdcard_service_t;

static portMUX_TYPE s_sdcard_mutex_guard = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_sdcard_mutex_storage;

static const sdcard_pin_config_t s_sdcard_pins = {
    .clk = GPIO_NUM_11,
    .cmd = GPIO_NUM_12,
    .d0 = GPIO_NUM_10,
    .d1 = GPIO_NUM_9,
    .d2 = GPIO_NUM_13,
    .d3 = GPIO_NUM_14,
    .width = 4,
};

static const esp_vfs_fat_sdmmc_mount_config_t s_mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024,
};

static sdcard_service_t s_sdcard = {
    .mutex = NULL,
    .host = {0},
    .slot = {0},
    .card = NULL,
    .initialized = false,
    .mounted = false,
};

static esp_err_t sdcard_ensure_mutex(void)
{
    if (s_sdcard.mutex != NULL) {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_sdcard_mutex_guard);
    if (s_sdcard.mutex == NULL) {
        s_sdcard.mutex = xSemaphoreCreateMutexStatic(&s_sdcard_mutex_storage);
    }
    taskEXIT_CRITICAL(&s_sdcard_mutex_guard);

    if (s_sdcard.mutex == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t sdcard_lock(void)
{
    const esp_err_t err = sdcard_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_sdcard.mutex, SD_CARD_ACCESS_TIMEOUT_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void sdcard_unlock(void)
{
    if (s_sdcard.mutex != NULL) {
        (void)xSemaphoreGive(s_sdcard.mutex);
    }
}

static void sdcard_fill_default_host_slot(void)
{
    s_sdcard.host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    s_sdcard.host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    s_sdcard.slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    s_sdcard.slot.clk = s_sdcard_pins.clk;
    s_sdcard.slot.cmd = s_sdcard_pins.cmd;
    s_sdcard.slot.d0 = s_sdcard_pins.d0;
    s_sdcard.slot.d1 = s_sdcard_pins.d1;
    s_sdcard.slot.d2 = s_sdcard_pins.d2;
    s_sdcard.slot.d3 = s_sdcard_pins.d3;
    s_sdcard.slot.width = s_sdcard_pins.width;
}

static esp_err_t sdcard_init_locked(void)
{
    if (s_sdcard.initialized) {
        return ESP_OK;
    }

    sdcard_fill_default_host_slot();
    s_sdcard.initialized = true;
    return ESP_OK;
}

static esp_err_t sdcard_mount_locked(bool quiet)
{
    if (s_sdcard.mounted) {
        return ESP_OK;
    }

    esp_err_t err = sdcard_init_locked();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT,
        &s_sdcard.host,
        &s_sdcard.slot,
        &s_mount_config,
        &s_sdcard.card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            /* Card answered card-init but FATFS could not mount it — a corrupt/garbage
             * filesystem, NOT an absent card. This silently bricks the node (main.lua +
             * the event log both live on SD), so surface it distinctly even when the
             * monitor calls quiet, and count it in NVS for post-mortem (audit A1).
             * format_if_mount_failed stays false so field data is never auto-wiped. */
            sdcard_note_corrupt_mount();
        } else if (!quiet) {
            ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        }
        s_sdcard.card = NULL;
        return err;
    }

    s_sdcard.mounted = true;
    sdcard_io_reset();          /* a clean (re)mount re-arms writers — not only the monitor path (audit E4) */

    /* Log card identity + capacity once per mount: a bad batch is traceable and a
     * counterfeit fake-capacity card leaves a breadcrumb (audit J5). */
    if (s_sdcard.card != NULL) {
        uint64_t cap_mb = ((uint64_t)s_sdcard.card->csd.capacity *
                           s_sdcard.card->csd.sector_size) >> 20;
        ESP_LOGI(TAG, "SD mounted: '%s' %lluMB serial=0x%08x",
                 s_sdcard.card->cid.name, (unsigned long long)cap_mb,
                 (unsigned)s_sdcard.card->cid.serial);
    }
    return ESP_OK;
}

/* Free bytes on the mounted FATFS volume (audit C1/C2/C4 — distinguishes a full
 * card from a dead one and feeds the storage-full watermark + heartbeat telemetry). */
esp_err_t sdcard_free_bytes(uint64_t *out_free)
{
    if (out_free == NULL) return ESP_ERR_INVALID_ARG;
    /* f_getfree walks the live volume — gate it so a concurrent unmount can't free
     * the volume under it (audit R-11); the in-store callers are already bracketed,
     * this covers the STATUS-heartbeat call. */
    if (!sdcard_io_begin()) return ESP_ERR_INVALID_STATE;
    uint64_t total = 0, freeb = 0;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb);
    if (err == ESP_OK) *out_free = freeb;
    sdcard_io_end();
    return err;
}

/* SDMMC serial (CID) of the mounted card, 0 if not mounted. Lets persistence detect
 * a card swap and avoid applying a stale NVS read-cursor to a different card's log
 * (audit I2/I3 — the failure class behind the ~35k-record gap). */
uint32_t sdcard_card_serial(void)
{
    uint32_t serial = 0;
    if (sdcard_lock() != ESP_OK) return 0;
    if (s_sdcard.mounted && s_sdcard.card != NULL) serial = s_sdcard.card->cid.serial;
    sdcard_unlock();
    return serial;
}

/* Log a "present-but-unmountable" (corrupt-FS) mount failure once per boot, so a
 * corrupt-card brick is diagnosable in the boot log (audit A1). Kept driver-local
 * (no NVS dependency); a persistent counter, if wanted, belongs in the app layer. */
static void sdcard_note_corrupt_mount(void)
{
    static bool logged_this_boot = false;
    if (logged_this_boot) return;
    logged_this_boot = true;
    ESP_LOGE(TAG, "SD present but FATFS unmountable (corrupt filesystem?) — persistence + "
                  "main.lua offline (format_if_mount_failed=false, so field data is preserved)");
}

esp_err_t sdcard_init_default(void)
{
    esp_err_t err = sdcard_lock();
    if (err != ESP_OK) {
        return err;
    }

    err = sdcard_init_locked();
    sdcard_unlock();
    return err;
}

esp_err_t sdcard_mount(void)
{
    esp_err_t err = sdcard_lock();
    if (err != ESP_OK) {
        return err;
    }

    err = sdcard_mount_locked(false);
    sdcard_unlock();
    return err;
}

esp_err_t sdcard_unmount(void)
{
    esp_err_t err = sdcard_lock();
    if (err != ESP_OK) {
        return err;
    }

    if (s_sdcard.mounted) {
        /* Drain in-flight FATFS ops before f_mount(NULL); never free the volume
         * under a live writer (audit F1). Callers reach this via app_prepare_reboot
         * AFTER the monitor is suspended + writers flushed/closed, so refs are 0 and
         * the drain returns immediately. */
        if (!sdcard_drain_and_unmount_locked("explicit unmount")) {
            err = ESP_ERR_TIMEOUT;   /* deferred: ops still in flight */
        }
    }

    sdcard_unlock();
    return err;
}

/* Lock-free advisory read (audit F3/F4). NEVER take the blocking mutex here: callers
 * poll this frequently and a mutex held for the ≤8 s teardown drain would wedge them.
 * A stale value is harmless — every real FS touch is authorized by sdcard_io_begin. */
bool sdcard_is_mounted(void)
{
    return s_sdcard.mounted;
}

/* ── Hot-plug monitor ─────────────────────────────────────────────────── */

static TaskHandle_t      s_monitor_task = NULL;
static sdcard_state_cb_t s_state_cb     = NULL;
static uint32_t          s_monitor_period_ms = 2000;   /* removal-detection probe (card in) */

/* Monitor stack (12 KB). Heap-allocated, NOT static/BSS: on this no-PSRAM board the
 * heap is tight (largest free block ~31 KB) and a permanent 12 KB static reservation
 * tips the marginal Lua-script load into ENOMEM. The monitor is created once at boot
 * while the heap is still clean (~126 KB free), so xTaskCreate can't realistically
 * fail there — the static-stack "insurance" (audit D4) costs more than it protects
 * against on this hardware. */
#define SD_MONITOR_STACK_BYTES   12288

/* While the card is OUT, retry quickly for a short window after loss (a human
 * reinserting a card expects recovery in seconds, not minutes), then back off to
 * a slow steady-state cap so a permanently-absent card costs almost nothing. */
#define SD_MONITOR_OUT_FAST_MS      3000                /* probe every 3 s … */
#define SD_MONITOR_OUT_FAST_WIN_MS  (60 * 1000)         /* … for the first minute after loss */
#define SD_MONITOR_OUT_RETRY_MS     (5 * 60 * 1000)     /* then every 5 minutes */

/* ── Error-driven card-loss latch (lock-free) ─────────────────────────────
 * The CMD13 poll cannot be trusted to notice a pulled card: it serializes on the
 * same SDMMC host mutex as the failing I/O and loses the race for s_sdcard.mutex
 * to a task stuck in a multi-second failing transfer. Instead, the FATFS writers
 * report each op's success/failure here; N consecutive failures latch "lost" and
 * wake the monitor to unmount NOW. Counter+flag live under a brief spinlock (never
 * the blocking s_sdcard.mutex — that is precisely what gets starved), so this path
 * stays usable exactly when the card is gone.
 *
 * Threshold > 1 on purpose: a marginal high-speed bus can throw a lone transient
 * 0x107 on a card that is still present; requiring a short run avoids unmounting
 * mid-measurement on a glitch while still reacting within a few seconds. */
#define SD_IO_FAIL_THRESHOLD  3

static portMUX_TYPE  s_io_lock        = portMUX_INITIALIZER_UNLOCKED;
static volatile int  s_io_fail_streak = 0;   /* volatile: read lock-free in the ok() fast-path */
static volatile bool s_sd_io_lost     = false;

/* FATFS RW-gate (audit F1): in-flight FATFS-op refcount + a teardown-pending flag.
 * A writer takes a ref (sdcard_io_begin) only while mounted, not lost, and no teardown
 * pending; teardown sets s_teardown_pending, waits for refs to hit 0, and only THEN
 * calls f_mount(NULL) — so the volume is never freed under a live op. If refs don't
 * drain, teardown DEFERS (retries next tick) rather than proceed. All three fields
 * live under s_io_lock (the brief spinlock, never the blocking s_sdcard.mutex). */
static volatile int  s_io_refs          = 0;
static volatile bool s_teardown_pending = false;

void sdcard_report_io_error(void)
{
    bool trip = false;
    portENTER_CRITICAL_SAFE(&s_io_lock);
    if (!s_sd_io_lost && ++s_io_fail_streak >= SD_IO_FAIL_THRESHOLD) {
        s_sd_io_lost = true;
        trip = true;
    }
    portEXIT_CRITICAL_SAFE(&s_io_lock);

    /* Wake the monitor to tear the mount down immediately (lock-free notify). */
    if (trip && s_monitor_task != NULL) {
        xTaskNotifyGive(s_monitor_task);
    }
}

void sdcard_report_io_ok(void)
{
    if (s_io_fail_streak == 0) return;          /* cheap fast-path (common case) */
    portENTER_CRITICAL_SAFE(&s_io_lock);
    if (!s_sd_io_lost) s_io_fail_streak = 0;    /* a real loss latch is sticky */
    portEXIT_CRITICAL_SAFE(&s_io_lock);
}

bool sdcard_io_lost(void)
{
    return s_sd_io_lost;
}

/* Re-arm after a successful (re)mount: clear the loss latch AND the RW-gate. Called
 * only when the card is known-good again — never while out, so writers stay gated
 * meanwhile. Resetting refs to 0 is safe because no op can span an unmount→remount
 * (teardown proved refs==0 before it freed the volume). Enforces the invariant
 * teardown_pending==true ⇒ card not usable (audit E4/F2/R-7). */
static void sdcard_io_reset(void)
{
    portENTER_CRITICAL_SAFE(&s_io_lock);
    s_io_fail_streak   = 0;
    s_sd_io_lost       = false;
    s_teardown_pending = false;
    s_io_refs          = 0;
    portEXIT_CRITICAL_SAFE(&s_io_lock);
}

/* RW-gate acquire/release (audit F1). A FATFS writer brackets its whole op:
 *   if (!sdcard_io_begin()) { bail — SD going away }
 *   ...fopen/fwrite/fsync/remove...
 *   sdcard_io_end();
 * begin() fails (returns false, no ref taken) if the card is unmounted, lost, or a
 * teardown is pending. Lock-free (brief spinlock), safe from any task. */
bool sdcard_io_begin(void)
{
    bool ok = false;
    portENTER_CRITICAL_SAFE(&s_io_lock);
    if (!s_teardown_pending && !s_sd_io_lost && s_sdcard.mounted) {
        s_io_refs++;
        ok = true;
    }
    portEXIT_CRITICAL_SAFE(&s_io_lock);
    return ok;
}

void sdcard_io_end(void)
{
    portENTER_CRITICAL_SAFE(&s_io_lock);
    configASSERT(s_io_refs > 0);          /* catch a begin/end imbalance in test */
    if (s_io_refs > 0) s_io_refs--;
    portEXIT_CRITICAL_SAFE(&s_io_lock);
}

/* Suspend the hot-plug monitor so it cannot run a teardown concurrently with the
 * pre-reboot flush/unmount (audit R-9). Called first in the shutdown handler. */
void sdcard_monitor_suspend(void)
{
    if (s_monitor_task != NULL) vTaskSuspend(s_monitor_task);
}

/* CMD13 poll debounce: a marginal HIGHSPEED bus can throw a lone transient CMD13
 * error on a still-present card, so require two consecutive bad polls before tearing
 * the mount down (the error-driven latch stays the fast real-pull path) — audit E1. */
static int s_probe_fail_streak = 0;

/* Drain in-flight FATFS ops, then unmount + latch loss. The gate (teardown_pending)
 * is raised FIRST under s_io_lock so no new op can start; then we wait for in-flight
 * refs to drain. Only when refs==0 do we call f_mount(NULL) — the volume is NEVER
 * freed under a live writer (audit F1/R-1/R-2). If refs don't drain within the budget
 * we DEFER: leave the mount up + teardown_pending set (writers stay gated) and return
 * false; the monitor's next tick retries, and a stuck driver op self-clears in ~5 s.
 * Caller holds s_sdcard.mutex. Returns true iff the volume was actually unmounted. */
#define SD_TEARDOWN_DRAIN_TICKS  800   /* ×10 ms = 8 s, > the ~5 s driver-op timeout */
static bool sdcard_drain_and_unmount_locked(const char *reason)
{
    portENTER_CRITICAL_SAFE(&s_io_lock);
    s_teardown_pending = true;                 /* gate new ops out (authoritative) */
    portEXIT_CRITICAL_SAFE(&s_io_lock);

    int refs = 0;
    for (int i = 0; i < SD_TEARDOWN_DRAIN_TICKS; i++) {
        portENTER_CRITICAL_SAFE(&s_io_lock);
        refs = s_io_refs;
        portEXIT_CRITICAL_SAFE(&s_io_lock);
        if (refs == 0) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (refs != 0) {
        ESP_LOGW(TAG, "SD teardown (%s) DEFERRED — %d FATFS op(s) still in flight, retrying",
                 reason, refs);
        return false;                          /* do NOT free the volume under a live op */
    }

    ESP_LOGW(TAG, "card lost (%s) — unmounting", reason);
    esp_err_t u = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_sdcard.card);
    if (u != ESP_OK) ESP_LOGW(TAG, "unmount after card-loss: %s", esp_err_to_name(u));
    s_sdcard.card    = NULL;
    s_sdcard.mounted = false;
    /* Latch loss so writers stay gated until a successful remount (which calls
     * sdcard_io_reset → clears both s_sd_io_lost and s_teardown_pending). */
    portENTER_CRITICAL_SAFE(&s_io_lock);
    s_sd_io_lost = true;
    portEXIT_CRITICAL_SAFE(&s_io_lock);
    return true;
}

/* One probe step: try to detect a state transition. Returns the new mounted
 * state (or current state if no change). Takes the lock briefly. */
static bool sdcard_probe_step(void)
{
    if (sdcard_lock() != ESP_OK) {
        return s_sdcard.mounted;     /* lock busy — report last known */
    }

    bool was_mounted = s_sdcard.mounted;

    if (was_mounted && s_sdcard.card != NULL) {
        if (s_sd_io_lost) {
            /* Error-driven loss wins over the poll: a writer already latched repeated
             * I/O failures — tear down now (drained; deferred+retried next tick if an
             * op is still in flight), skipping the CMD13 probe. */
            (void)sdcard_drain_and_unmount_locked("I/O errors");
            s_probe_fail_streak = 0;
        } else {
            /* CMD13 stays UNDER s_sdcard.mutex (audit R-3): moving it off-mutex would
             * race an unmount freeing s_sdcard.card. A healthy poll is sub-10 ms; the
             * slow (pulled-card) case is short-circuited by the s_sd_io_lost branch. */
            esp_err_t err = sdmmc_get_status(s_sdcard.card);
            if (err != ESP_OK) {
                if (++s_probe_fail_streak >= 2) {   /* debounce a lone transient CMD13 */
                    if (sdcard_drain_and_unmount_locked(esp_err_to_name(err))) {
                        s_probe_fail_streak = 0;
                    }   /* else deferred → keep streak so the next tick retries at once */
                } else {
                    ESP_LOGW(TAG, "CMD13 probe failed (%s) — retrying before unmount",
                             esp_err_to_name(err));
                }
            } else {
                s_probe_fail_streak = 0;             /* healthy poll → reset debounce */
            }
        }
    } else if (!was_mounted) {
        /* Card was out — try to remount. Quiet on failure (likely no card).
         * sdcard_mount_locked() now clears the io-loss latch itself on success. */
        esp_err_t err = sdcard_mount_locked(true);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "card detected — remounted");
        }
    }

    bool now_mounted = s_sdcard.mounted;
    sdcard_unlock();
    return (now_mounted != was_mounted) ? now_mounted : was_mounted;
    /* Caller compares against its own previous state to fire transitions. */
}

static void sdcard_monitor_task(void *arg)
{
    (void)arg;
    bool       last     = sdcard_is_mounted();
    TickType_t out_since = 0;            /* tick the card last went OUT (adaptive retry) */

    /* Stagger the first probe so we don't fight boot-time SD activity. */
    vTaskDelay(pdMS_TO_TICKS(s_monitor_period_ms));

    while (1) {
        bool now = sdcard_probe_step();
        if (now != last) {
            ESP_LOGI(TAG, "state transition: %s -> %s",
                     last ? "mounted" : "out",
                     now  ? "mounted" : "out");
            if (!now) out_since = xTaskGetTickCount();
            if (s_state_cb != NULL) {
                s_state_cb(now);
            }
            last = now;
        }

        /* Card in → probe periodically (the error-driven notify catches a pull
         * faster than this). Card out → retry remount quickly for the first
         * minute (human reinsert), then back off to the slow steady-state cap. */
        TickType_t delay;
        if (now) {
            delay = pdMS_TO_TICKS(s_monitor_period_ms);
        } else {
            bool fast = (xTaskGetTickCount() - out_since) < pdMS_TO_TICKS(SD_MONITOR_OUT_FAST_WIN_MS);
            delay = pdMS_TO_TICKS(fast ? SD_MONITOR_OUT_FAST_MS : SD_MONITOR_OUT_RETRY_MS);
        }
        /* Sleep until the delay elapses OR a writer signals card-loss — whichever
         * first — so a pulled card is torn down within ~a failed op, not a poll. */
        ulTaskNotifyTake(pdTRUE, delay);
    }
}

esp_err_t sdcard_start_monitor(uint32_t period_ms, sdcard_state_cb_t cb)
{
    if (s_monitor_task != NULL) {
        return ESP_OK;     /* already running */
    }
    if (period_ms < 200) period_ms = 200;
    s_monitor_period_ms = period_ms;
    s_state_cb          = cb;

    /* Fully silence the ESP-IDF SDMMC layers. Two reasons: (1) they log the failed
     * card-init at ERROR on every remount retry while the card is out; (2) — more
     * important — when a mounted card is pulled, sdmmc_cmd/diskio_sdmmc/sdmmc_req
     * emit an ERROR per failed sector op, and those lines are tee'd by sd_logger
     * back onto the (gone) SD, re-arming yet more failing I/O. Silencing the four
     * tags breaks that feedback loop. The sd_card component reports mount state
     * through its own logs, so we lose no useful visibility. */
    esp_log_level_set("sdmmc_common",  ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd",     ESP_LOG_NONE);
    esp_log_level_set("diskio_sdmmc",  ESP_LOG_NONE);
    esp_log_level_set("sdmmc_req",     ESP_LOG_NONE);

    /* 12 KB stack: the remount path goes through esp_vfs_fat_sdmmc_mount +
     * FATFS, and the cb fans out to event_log_on_sd_restored() + lua_runner_start()
     * (FATFS reopen is a heavy stack user). 4 KB overflowed; 8 KB was marginal.
     * Static (BSS) storage so a fragmented heap can never fail to start the only
     * task that can recover a lost card (audit D4). */
    BaseType_t ok = xTaskCreate(sdcard_monitor_task, "sd_monitor",
                                SD_MONITOR_STACK_BYTES, NULL, 2, &s_monitor_task);
    if (ok != pdPASS) {
        s_monitor_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "hot-plug monitor started (period=%u ms)", (unsigned)period_ms);
    return ESP_OK;
}

#include "device_commands.h"
#include "event_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ambit_protocol.h"
#include "clock_trust.h"
#include "timezone.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "dev_cmd"

#define MQTT_TOPIC_MAX        256
#define MQTT_PAYLOAD_MAX     (128U * 1024U)   /* AWS IoT max payload */

/* Bench-gated large-publish ceiling. The default derives from the normal store
 * cap plus 4 KiB of JSON-envelope headroom, so producer/store changes cannot
 * silently make a valid record hit the publish-policy archive path. A
 * conservative field build can set
 * -DAMBYTE_PUBLISH_MAX_BYTES=16384 until chunking exists. This gate is distinct
 * from the larger store cap: lowering it archives an over-cap event to the
 * event-log quarantine sidecar, increments s_oversize_skipped, and advances the
 * FIFO instead of silently dropping data or head-of-line blocking forever. */
#ifndef AMBYTE_PUBLISH_MAX_BYTES
#define AMBYTE_PUBLISH_MAX_BYTES (EVLOG_RECORD_CAP_NORMAL + 4096U)
#define AMBYTE_PUBLISH_CAP_IS_DEFAULT 1
#else
#define AMBYTE_PUBLISH_CAP_IS_DEFAULT 0
#endif

#define DC_EVENT_ENVELOPE_FMT \
        "{\"sample\":[{" \
            "\"v\":2,\"measure_id\":%lld,\"startTicks_UTC\":%lld,\"endTicks_UTC\":%lld," \
            "\"timestamp_local\":\"%s\"," \
            "\"published\":\"%s\",\"channel\":%s,\"device\":%s," \
            "\"cmd_raw\":%s,\"tag\":\"%s\",\"metadata\":%s,\"data\":%s" \
        "}]," \
        "\"timestamp\":\"%s\",%s%s" \
        "\"device_id\":\"%s\",\"device_name\":\"%s\"," \
        "\"device_version\":\"%s\",\"device_firmware\":\"%s\"}"

/* Large-publish heap gate (Track 1) — see the gate in cmd_mqtt_publish_next_event.
 * An arrun envelope can now approach 64 KiB and needs an outbox copy plus a
 * transient mbedTLS write buffer. The large mallocs route to PSRAM, while the
 * internal/DMA pools still need contiguous TLS/lwIP headroom. */
#define DC_LARGE_PUBLISH_BYTES     3072U   /* envelopes above this get the heap gate */
#define DC_PUBLISH_HEAP_HEADROOM   2048U   /* floor slack atop the envelope copy (payload_json is freed pre-publish; TLS write ~2.4 KB) */
#define DC_PUBLISH_SETTLE_MS       300U    /* let the requested GC + transient frees land before re-checking */
/* Poison-event escape: after this many CONSECUTIVE heap-defers/publish-failures
 * of the SAME event, quarantine it (archive to SD + advance the cursor) so one
 * un-publishable record can't head-of-line-block the strict FIFO forever.
 * ~30 attempts ≈ 5 min at the observed drain cadence. Field-proven necessary
 * (2026-07-09): a 5.4 KB old-schema trace deferred 2092x over 5.6 h, wedging
 * ~8300 events behind it on a unit whose largest free block never recovers. */
#define DC_PUBLISH_STUCK_MAX       30

/* Window 16 + a full disconnect flush + margin. Producers still drop the newest
 * completion on overflow: the corresponding latch is restored, so its event-log
 * slot remains unadvanced and is eventually reaped/replayed. */
#define DC_ACK_QUEUE_DEPTH         32

_Static_assert(PUBLISH_WINDOW_SLOTS > 0 && PUBLISH_WINDOW_SLOTS <= 16,
               "device-command ACK table supports at most 16 publish slots");
_Static_assert(DC_ACK_QUEUE_DEPTH >= (2U * PUBLISH_WINDOW_SLOTS),
               "ACK queue must hold a full window plus disconnect-flush headroom");
_Static_assert(AMBYTE_PUBLISH_MAX_BYTES > 0 && AMBYTE_PUBLISH_MAX_BYTES < MQTT_PAYLOAD_MAX,
               "publish cap must be non-zero and below the broker payload limit");
#if AMBYTE_PUBLISH_CAP_IS_DEFAULT
_Static_assert(EVLOG_RECORD_CAP_NORMAL < AMBYTE_PUBLISH_MAX_BYTES,
               "default publish cap must exceed the normal store cap");
#endif

static device_commands_config_t s_cfg;
static bool s_initialized = false;
static char s_mac_str[18]; /* "XX:XX:XX:XX:XX:XX\0" */

/* In-flight publish tracking. This spinlock protects ONLY the small RAM table:
 * no allocation, logging, MQTT call, event-log call, or other I/O may occur
 * inside it. Consequently the esp-mqtt task can resolve msg_id -> measure_id in
 * a bounded critical section even while event_log's unrelated s_mtx is held for
 * seconds by FATFS. Task context only: this path is NOT
 * ISR-safe (portENTER_CRITICAL plus xQueueSend, not the FromISR variants). */
static portMUX_TYPE s_inflight_mtx = portMUX_INITIALIZER_UNLOCKED;

/* msg_id < 0 means the slot is reserved around esp_mqtt_client_publish(). The
 * reservation is installed BEFORE the synchronous socket write begins. If a
 * sub-ms PUBACK races the publish return on the other core, the ACK callback
 * parks it under the same lock; finalization resolves the returned id and emits
 * exactly one completion. */
typedef struct {
    bool      used;
    int64_t   measure_id;
    int       msg_id;
    int64_t   since_ms;
    size_t    envelope_bytes;
} dc_inflight_slot_t;

static dc_inflight_slot_t s_inflight[PUBLISH_WINDOW_SLOTS];

/* Only sync_runner calls publish, so at most one negative-id reservation exists.
 * Keep two unmatched callbacks beside that reservation: a command/status publish
 * can be ACKed in the same API-unlock gap immediately before OUR fast PUBACK.
 * One park entry dropped the second callback and recreated the 60 s duplicate;
 * two entries distinguish the unrelated id once publish() returns ours. */
#define DC_EARLY_ACK_PARKS 2U
typedef struct {
    int       msg_id;
    esp_err_t status;
} dc_early_ack_t;
static dc_early_ack_t s_early_acks[DC_EARLY_ACK_PARKS];
static size_t         s_early_ack_count;
/* Monotonic ms of the last successful PUBACK (end-to-end publish success). The
 * connectivity watchdog uses "time since" this to detect a device that should be
 * publishing but can't, and reboot it. Seeded to boot time in init. */
static int64_t s_last_publish_ok_ms  = 0;

typedef enum {
    DC_ACK_PUBACK = 0,
    DC_ACK_PUBLISH_ERROR,
    DC_ACK_DISCONNECT,
} dc_ack_kind_t;

/* Completion crosses from the esp-mqtt task to the sole sync-runner consumer.
 * Carry measure_id as well as msg_id: the producer resolves the volatile latch
 * under s_inflight_mtx, while ONLY the consumer is allowed to touch event_log. */
typedef struct {
    int64_t       measure_id;
    int           msg_id;
    esp_err_t     status;
    dc_ack_kind_t kind;
} dc_ack_completion_t;

static QueueHandle_t s_ack_queue = NULL;
static StaticQueue_t s_ack_queue_control;
static dc_ack_completion_t s_ack_queue_storage[DC_ACK_QUEUE_DEPTH];
/* Producer-side overflow telemetry. Incremented under s_inflight_mtx; drained,
 * cleared, and logged only by sync_runner so esp-mqtt never pays console latency. */
static uint32_t s_ack_drops = 0;

/* Genuine per-record failure streaks for the event-log window. Only the sole
 * sync-runner task touches this table. Success for B clears B alone; A's
 * poison-record evidence survives interleaving and can quarantine only when A
 * reaches the cursor frontier. */
typedef struct {
    int64_t measure_id;
    uint8_t failures;
} dc_stuck_slot_t;
static dc_stuck_slot_t s_stuck[PUBLISH_WINDOW_SLOTS];
/* Publish-cap skips are intentionally separate from poison-record retry counts:
 * these records are valid and preserved on SD, merely unsupported by this
 * build's link-size policy. Sole sync-runner writer, so no lock is required. */
static uint32_t s_oversize_skipped = 0;
static int64_t  s_oversize_warned_id = 0;

/* Monotonic milliseconds since boot — independent of the wall clock (which jumps
 * on RTC sync), so it measures elapsed in-flight time correctly. */
static inline int64_t mono_ms(void) { return esp_timer_get_time() / 1000; }

/* All helpers with `_locked` in the name require s_inflight_mtx. */
static int inflight_find_free_locked(void)
{
    for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
        if (!s_inflight[i].used) return (int)i;
    }
    return -1;
}

static int inflight_find_msg_locked(int msg_id)
{
    for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
        if (s_inflight[i].used && s_inflight[i].msg_id == msg_id) return (int)i;
    }
    return -1;
}

static void inflight_clear_locked(size_t idx)
{
    memset(&s_inflight[idx], 0, sizeof s_inflight[idx]);
}

static void early_acks_clear_locked(void)
{
    memset(s_early_acks, 0, sizeof s_early_acks);
    s_early_ack_count = 0;
}

static void inflight_usage_locked(size_t *slots, size_t *bytes)
{
    size_t n = 0, b = 0;
    for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
        if (!s_inflight[i].used) continue;
        n++;
        b += s_inflight[i].envelope_bytes;
    }
    if (slots != NULL) *slots = n;
    if (bytes != NULL) *bytes = b;
}

/* Measurement activity spans both the Lua whole-cycle bracket and each raw
 * sensor transaction. It owns the PM no-light-sleep lock and telemetry signal.
 * The narrower publish hold below covers only raw transactions, allowing MQTT
 * publishing during the rest of a measurement routine. Both are counters (not
 * bools) so nested brackets don't clear them early. Single 32-bit access is
 * atomic on the ESP32; the sync runner only reads them. */
static volatile int s_measurement_active = 0;
static volatile int s_publish_hold = 0;
/* Phase 2: held across a measurement window so the SoC can't light-sleep mid
 * AMBIT UART / I2C read. NULL if PM is disabled (begin/end then no-op). */
static esp_pm_lock_handle_t s_no_ls_lock = NULL;

static cmd_result_t make_result(esp_err_t status, const char *fmt, ...)
{
    cmd_result_t r;
    r.status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

/* Append one optional STATUS block transactionally. The sizing pass avoids a
 * scratch buffer: if the entire block plus the final '}' will not fit, leave
 * the payload untouched, log exactly one warning for that block, and let the
 * heartbeat continue with the remaining optional blocks. */
static bool status_append_optional(char *payload, size_t cap, size_t *len,
                                   const char *block, const char *fmt, ...)
{
    va_list ap, sizing;
    va_start(ap, fmt);
    va_copy(sizing, ap);
    int needed = vsnprintf(NULL, 0, fmt, sizing);
    va_end(sizing);

    size_t available = (*len + 2 <= cap) ? cap - *len - 2 : 0;
    if (needed < 0 || (size_t)needed > available) {
        ESP_LOGW(TAG, "STATUS optional block '%s' omitted (needs=%d, available=%u)",
                 block, needed, (unsigned)available);
        va_end(ap);
        return false;
    }

    int wrote = vsnprintf(payload + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (wrote != needed) {
        payload[*len] = '\0';
        ESP_LOGW(TAG, "STATUS optional block '%s' omitted (format failed)", block);
        return false;
    }
    *len += (size_t)wrote;
    return true;
}

/* STATUS string sources are controlled and short, but PROJECT_VER can still
 * contain punctuation supplied by the build. Keep the value JSON-safe without
 * allocating or introducing an escaping-size worst case. */
static void status_copy_json_safe(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t n = 0;
    while (src != NULL && src[n] != '\0' && n + 1 < cap) {
        unsigned char c = (unsigned char)src[n];
        dst[n] = (c >= 0x20 && c != '"' && c != '\\') ? (char)c : '_';
        n++;
    }
    dst[n] = '\0';
}

/* ── Sync-runner wake hook ───────────────────────────────────────────────
 * The sole publisher (sync_runner) registers a task-notify here so it can sleep
 * until there's work instead of polling. Fired after every stored event, after
 * a raw publish hold clears, and when a measurement burst finishes. */
static void (*s_sync_notifier)(void) = NULL;

static void device_commands_publish_hold_begin(void);
static void device_commands_publish_hold_end(void);
static void sensor_transaction_begin(void);
static void sensor_transaction_end(void);

void device_commands_set_sync_notifier(void (*fn)(void)) { s_sync_notifier = fn; }

static inline void notify_sync(void)
{
    void (*fn)(void) = s_sync_notifier;   /* single read; set once at boot */
    if (fn != NULL) fn();
}

/* Non-blocking producer shared by PUBACK/error and disconnect callbacks. Queue
 * send/receive use zero wait: socket servicing must never wait for the drain.
 * Thirty-two entries cover a full ACK window plus a concurrent full disconnect
 * flush. If a future change violates that capacity contract, drop the NEW
 * completion without ever receiving from this producer task. Its latch is then
 * restored, so the record remains unadvanced and eventually replays; this
 * fail-closed policy can duplicate data but cannot skip it. */
static bool enqueue_ack_completion(const dc_ack_completion_t *completion)
{
    if (s_ack_queue == NULL) return false;

    if (xQueueSend(s_ack_queue, completion, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_inflight_mtx);
        if (s_ack_drops != UINT32_MAX) s_ack_drops++;
        portEXIT_CRITICAL(&s_inflight_mtx);
        notify_sync();  /* drain logs the drop and re-claims the unadvanced record */
        return false;
    }

    /* Enqueue happens-before the wake: a runner that returned after its PUBACK
     * poll cap will see and apply this completion before it attempts a claim. */
    notify_sync();
    return true;
}

/* Internal ack handler — called by mqtt_client on its esp-mqtt task. This
 * function deliberately does only a bounded RAM-latch lookup, a zero-wait queue
 * send, and a task notify. In particular it must never call an s_cfg event-log
 * function: those take event_log's s_mtx across potentially 5 s FATFS I/O. */
static void on_publish_ack(int msg_id, esp_err_t status, void *ctx)
{
    (void)ctx;
    dc_ack_completion_t completion = {
        .measure_id = -1,
        .msg_id     = msg_id,
        .status     = status,
        .kind       = status == ESP_OK ? DC_ACK_PUBACK : DC_ACK_PUBLISH_ERROR,
    };

    bool matched = false;
    bool parked = false;
    bool park_overflow = false;
    int matched_idx = -1;
    dc_inflight_slot_t detached = {0};
    int64_t acked_at_ms = status == ESP_OK ? mono_ms() : 0;
    portENTER_CRITICAL(&s_inflight_mtx);
    matched_idx = inflight_find_msg_locked(msg_id);
    if (matched_idx >= 0) {
        detached = s_inflight[matched_idx];
        completion.measure_id = detached.measure_id;
        inflight_clear_locked((size_t)matched_idx);
        if (status == ESP_OK) s_last_publish_ok_ms = acked_at_ms;
        matched = true;
    } else {
        /* esp_mqtt_client_publish unlocks its API mutex just before returning.
         * On the other core, a local/sub-ms broker can therefore deliver PUBACK
         * while our slot still carries the reserved msg_id=-1. There is only one
         * publisher (sync_runner), hence at most one reservation. Preserve up to
         * two distinct early ids so an unrelated raw-publish ACK cannot consume
         * the only park position ahead of ours. */
        for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
            if (!s_inflight[i].used || s_inflight[i].msg_id >= 0) continue;
            for (size_t j = 0; j < s_early_ack_count; j++) {
                if (s_early_acks[j].msg_id == msg_id) {
                    parked = true;  /* duplicate callback; one completion suffices */
                    break;
                }
            }
            if (!parked && s_early_ack_count < DC_EARLY_ACK_PARKS) {
                s_early_acks[s_early_ack_count++] = (dc_early_ack_t) {
                    .msg_id = msg_id,
                    .status = status,
                };
                parked = true;
            } else if (!parked) {
                park_overflow = true;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_inflight_mtx);

    if (park_overflow) {
        /* A third distinct callback in the publish() API-unlock gap is not
         * correlatable with the single reservation. Preserve the fail-closed
         * behavior (60 s reap + duplicate) but leave a field/bench breadcrumb. */
        ESP_LOGW(TAG, "early-ACK park full: dropped distinct msg_id=%d status=%s; reservation will replay",
                 msg_id, esp_err_to_name(status));
    }
    if (parked || !matched) return;
    if (!enqueue_ack_completion(&completion)) {
        /* Preserve fail-closed replay on the structurally-unexpected overflow:
         * restore the mutual-exclusion token instead of losing both latch and
         * completion. A later duplicate PUBACK or the 60-s reaper retries it. */
        portENTER_CRITICAL(&s_inflight_mtx);
        int restore_idx = !s_inflight[matched_idx].used
            ? matched_idx : inflight_find_free_locked();
        if (restore_idx >= 0) s_inflight[restore_idx] = detached;
        portEXIT_CRITICAL(&s_inflight_mtx);
    }
}

/* Adapter so the MQTT transport's disconnect callback (message_disconnect_fn)
 * can drive the same in-flight-clear as the Wi-Fi disconnect path. */
static void on_mqtt_disconnect_cb(void *ctx)
{
    (void)ctx;
    device_commands_on_mqtt_disconnect();
}

esp_err_t device_commands_init(const device_commands_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The delivery transition is a three-function contract. Accept persistence
     * fully wired or fully absent, never a partial setup where a successful
     * PUBACK could accidentally fall through to mark_event_pending. */
    bool any_delivery_fn = cfg->claim_next_event != NULL ||
                           cfg->mark_event_synced != NULL ||
                           cfg->mark_event_pending != NULL;
    bool all_delivery_fns = cfg->claim_next_event != NULL &&
                            cfg->mark_event_synced != NULL &&
                            cfg->mark_event_pending != NULL;
    if (any_delivery_fn && !all_delivery_fns) {
        ESP_LOGE(TAG, "incomplete persistence delivery callbacks");
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    if (s_ack_queue == NULL) {
        s_ack_queue = xQueueCreateStatic(DC_ACK_QUEUE_DEPTH,
                                         sizeof(dc_ack_completion_t),
                                         (uint8_t *)s_ack_queue_storage,
                                         &s_ack_queue_control);
        if (s_ack_queue == NULL) return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_inflight_mtx);
    memset(s_inflight, 0, sizeof s_inflight);
    early_acks_clear_locked();
    s_ack_drops = 0;
    portEXIT_CRITICAL(&s_inflight_mtx);
    memset(s_stuck, 0, sizeof s_stuck);
    xQueueReset(s_ack_queue);
    s_initialized = true;
    /* Start the watchdog "last success" clock at boot, so a device that boots
     * with a backlog and never connects is given the full timeout before reboot. */
    s_last_publish_ok_ms = mono_ms();

    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(s_mac_str, sizeof(s_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        s_mac_str[0] = '\0';
    }

    if (s_cfg.set_publish_ack_handler != NULL) {
        s_cfg.set_publish_ack_handler(on_publish_ack, NULL);
    }
    /* Revert the in-flight window on an MQTT-level disconnect too, not just a
     * Wi-Fi drop — otherwise broker/TLS loss with Wi-Fi up wedges the drain. */
    if (s_cfg.set_disconnect_handler != NULL) {
        s_cfg.set_disconnect_handler(on_mqtt_disconnect_cb, NULL);
    }

    /* PM lock for the measurement window (Phase 2). Requires CONFIG_PM_ENABLE;
     * if PM is off this returns an error and the begin/end hooks stay no-ops. */
    if (s_no_ls_lock == NULL) {
        esp_err_t lerr = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                                            "ambit_meas", &s_no_ls_lock);
        if (lerr != ESP_OK) {
            s_no_ls_lock = NULL;
            ESP_LOGW(TAG, "PM lock unavailable (%s) — measurement won't block light sleep",
                     esp_err_to_name(lerr));
        }
    }

    ESP_LOGI(TAG, "Device commands initialized");
    ESP_LOGI(TAG, "  MAC:            %s", s_mac_str[0] ? s_mac_str : "(unavail)");
    ESP_LOGI(TAG, "  topic_root:     %s", s_cfg.topic_root      ? s_cfg.topic_root      : "(null)");
    ESP_LOGI(TAG, "  device_id:      %s", s_cfg.device_id       ? s_cfg.device_id       : "(null)");
    ESP_LOGI(TAG, "  protocol_id:    %s", s_cfg.protocol_id     ? s_cfg.protocol_id     : "(null)");
    ESP_LOGI(TAG, "  device_name:    %s", s_cfg.device_name     ? s_cfg.device_name     : "(null)");
    ESP_LOGI(TAG, "  device_version: %s", s_cfg.device_version  ? s_cfg.device_version  : "(null)");
    ESP_LOGI(TAG, "  device_firm:    %s", s_cfg.device_firmware ? s_cfg.device_firmware : "(null)");
    ESP_LOGI(TAG, "  timezone:       %s", (s_cfg.timezone && s_cfg.timezone[0]) ? s_cfg.timezone : "(unset)");
    return ESP_OK;
}

esp_err_t cmd_process_pending_acks(void)
{
    if (s_ack_queue == NULL) return ESP_ERR_INVALID_STATE;

    uint32_t ack_drops;
    portENTER_CRITICAL(&s_inflight_mtx);
    ack_drops = s_ack_drops;
    s_ack_drops = 0;
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (ack_drops > 0) {
        ESP_LOGE(TAG, "ack queue full: dropped %u newest completion(s); "
                      "records remain unadvanced and will replay",
                 (unsigned)ack_drops);
    }

    dc_ack_completion_t completion;
    while (xQueuePeek(s_ack_queue, &completion, 0) == pdTRUE) {
        /* Sole-consumer contract: sync_runner is the only task that calls this
         * function. Applying PENDING/SYNCED here serializes completion with the
         * next claim and with note_publish_stuck(), so a deferred failure cannot
         * race a re-claim of the same measure_id. This is the synchronization;
         * do not move event-log calls back into an MQTT callback when ticket 05
         * replaces the scalar latch with a window. */
        esp_err_t err;
        if (completion.status == ESP_OK) {
            err = s_cfg.mark_event_synced != NULL
                ? s_cfg.mark_event_synced(completion.measure_id)
                : ESP_ERR_NOT_SUPPORTED;
        } else if (s_cfg.mark_event_pending != NULL) {
            err = s_cfg.mark_event_pending(completion.measure_id);
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
        }

        /* Init rejects a partial persistence callback set, so NOT_SUPPORTED is
         * unreachable in the normal configured drain. Keep it terminal anyway as
         * a defensive boundary for offline/test ports and future compositions. */
        bool terminal = err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_SUPPORTED;
        if (err != ESP_OK && !terminal) {
            /* TIMEOUT/I/O failures are transient: leave the head in place and
             * stop so the later wake/fallback retries without claiming around an
             * unapplied completion. */
            ESP_LOGW(TAG, "deferred %s msg_id=%d id=%lld apply failed (%s) — will retry",
                     completion.status == ESP_OK ? "PUBACK" : "pending-mark",
                     completion.msg_id, (long long)completion.measure_id,
                     esp_err_to_name(err));
            return err;
        }
        if (terminal) {
            /* Persistence reset/reopen deliberately abandons its RAM window.
             * INVALID_STATE means this id is already absent/advanced/reverted;
             * NOT_SUPPORTED means the backing store is offline. Retrying either
             * forever would pin the queue head and stop all future claims. Drop
             * the stale completion; the durable cursor is the replay authority. */
            ESP_LOGW(TAG, "dropping stale deferred %s msg_id=%d id=%lld after persistence reset (%s)",
                     completion.status == ESP_OK ? "PUBACK" : "pending-mark",
                     completion.msg_id, (long long)completion.measure_id,
                     esp_err_to_name(err));
        }

        dc_ack_completion_t removed;
        if (xQueueReceive(s_ack_queue, &removed, 0) != pdTRUE) {
            /* A persistence-reset/CLI-rewind callback may have reset the queue
             * after our peek. It also cleared every correlation latch, so there
             * is no completion left to preserve and claiming may safely resume. */
            ESP_LOGW(TAG, "ack queue reset while applying id=%lld; durable cursor will replay",
                     (long long)completion.measure_id);
            return ESP_OK;
        }
        if (!terminal) {
            ESP_LOGD(TAG, "processed deferred %s msg_id=%d id=%lld",
                     completion.kind == DC_ACK_PUBACK ? "PUBACK" :
                     (completion.kind == DC_ACK_DISCONNECT ? "disconnect" : "publish error"),
                     completion.msg_id, (long long)completion.measure_id);
        }
    }
    return ESP_OK;
}

const char *device_commands_get_mac(void)
{
    return s_mac_str;   /* "" if esp_wifi_get_mac failed at init */
}

cmd_result_t cmd_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized || s_cfg.set_status == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "status LED not available");
    }
    esp_err_t err = s_cfg.set_status(r, g, b);
    if (err != ESP_OK) {
        return make_result(err, "set_rgb failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "RGB set to (%u, %u, %u)", r, g, b);
}

/* ── PWM output (LEDC on GPIO4) ─────────────────────────────────────────
 *
 * A single LEDC timer/channel drives GPIO4. The duty resolution is chosen per
 * call as the largest that supports the requested frequency on the 80 MHz APB
 * clock (ESP32-S3 LEDC tops out at 14-bit). */
#define PWM_GPIO          4
#define PWM_TIMER         LEDC_TIMER_0
#define PWM_CHANNEL       LEDC_CHANNEL_0
#define PWM_SPEED_MODE    LEDC_LOW_SPEED_MODE   /* S3 has no high-speed mode */
#define PWM_SRC_CLK_HZ    80000000u             /* APB clock */
#define PWM_MAX_RES_BITS  14                    /* SOC_LEDC_TIMER_BIT_WIDTH (S3) */

static bool s_pwm_configured = false;

/* Largest duty resolution (bits) whose full-scale period fits freq_hz on the
 * APB clock. Returns 0 if the frequency is too high to represent. */
static int pwm_pick_resolution(uint32_t freq_hz)
{
    int bits = 0;
    while (bits < PWM_MAX_RES_BITS &&
           (1u << (bits + 1)) <= (PWM_SRC_CLK_HZ / freq_hz)) {
        bits++;
    }
    return bits;
}

cmd_result_t cmd_pwm(float duty_pct, uint32_t freq_hz, bool enable)
{
    if (duty_pct < 0.0f || duty_pct > 100.0f) {
        return make_result(ESP_ERR_INVALID_ARG,
                           "duty must be 0..100 (got %.2f)", (double)duty_pct);
    }

    if (!enable) {
        if (s_pwm_configured) {
            ledc_stop(PWM_SPEED_MODE, PWM_CHANNEL, 0);  /* hold pin low */
        }
        return make_result(ESP_OK, "PWM disabled on GPIO%d", PWM_GPIO);
    }

    if (freq_hz == 0) {
        return make_result(ESP_ERR_INVALID_ARG, "freq must be > 0");
    }
    int bits = pwm_pick_resolution(freq_hz);
    if (bits == 0) {
        return make_result(ESP_ERR_INVALID_ARG, "freq %u Hz too high (max %u Hz)",
                           (unsigned)freq_hz, (unsigned)(PWM_SRC_CLK_HZ / 2));
    }

    ledc_timer_config_t tcfg = {
        .speed_mode      = PWM_SPEED_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        return make_result(err, "ledc_timer_config(%u Hz, %d-bit) failed: %s",
                           (unsigned)freq_hz, bits, esp_err_to_name(err));
    }

    uint32_t full = 1u << bits;
    uint32_t raw  = (uint32_t)lroundf(duty_pct / 100.0f * (float)full);
    if (raw > full) {
        raw = full;
    }

    /* Re-running channel_config each enable is idempotent and re-asserts the
     * output after a prior disable (ledc_stop). */
    ledc_channel_config_t ccfg = {
        .gpio_num   = PWM_GPIO,
        .speed_mode = PWM_SPEED_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .duty       = raw,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        return make_result(err, "ledc_channel_config failed: %s",
                           esp_err_to_name(err));
    }
    s_pwm_configured = true;

    return make_result(ESP_OK, "PWM GPIO%d: %.2f%% @ %u Hz (%d-bit)",
                       PWM_GPIO, (double)duty_pct, (unsigned)freq_hz, bits);
}

cmd_result_t cmd_read_rtc(time_t *out_time)
{
    if (!s_initialized || s_cfg.read_clock == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "RTC not available");
    }
    if (out_time == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "out_time is NULL");
    }
    esp_err_t err = s_cfg.read_clock(out_time);
    if (err != ESP_OK) {
        return make_result(err, "RTC read failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "RTC: %lld", (long long)*out_time);
}

/* Sanity bounds for an externally-supplied clock (MQTT set_time). Reject anything
 * outside [2024-01-01, 2100-01-01) so a garbage/stale/replayed stamp — a never-set
 * 1970 clock, a corrupt payload, an absurd future — can't wreck the RTC. We do NOT
 * gate on distance from the current time: a wrong clock is the reason to call this,
 * so a freshness check would reject legitimate corrections (the current clock can't
 * be trusted). The operator must therefore NOT send set_time as a retained message. */
#define CMD_SET_RTC_MIN_EPOCH  1704067200LL   /* 2024-01-01T00:00:00Z */
#define CMD_SET_RTC_MAX_EPOCH  4102444800LL   /* 2100-01-01T00:00:00Z */

cmd_result_t cmd_set_rtc(int64_t epoch_utc)
{
    if (!s_initialized || s_cfg.set_clock == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "RTC set not available");
    }
    if (epoch_utc < CMD_SET_RTC_MIN_EPOCH || epoch_utc >= CMD_SET_RTC_MAX_EPOCH) {
        return make_result(ESP_ERR_INVALID_ARG,
                           "epoch %lld out of range [2024,2100)", (long long)epoch_utc);
    }
    esp_err_t err = s_cfg.set_clock((time_t)epoch_utc);
    if (err != ESP_OK) {
        return make_result(err, "RTC set failed: %s", esp_err_to_name(err));
    }
    clock_trust_note_rtc();
    char iso[32];
    struct tm tm_utc;
    time_t t = (time_t)epoch_utc;
    gmtime_r(&t, &tm_utc);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return make_result(ESP_OK, "%s", iso);
}

cmd_result_t cmd_device_status(bool *bme_ready, bool *rtc_ready, time_t *rtc_time)
{
    bool bme_ok = false;
    bool rtc_ok = false;

    if (s_initialized && s_cfg.read_env != NULL) {
        measurement_t m;
        bme_ok = (s_cfg.read_env(&m) == ESP_OK);
    }

    if (s_initialized && s_cfg.read_clock != NULL) {
        time_t t = 0;
        if (s_cfg.read_clock(&t) == ESP_OK) {
            rtc_ok = true;
            if (rtc_time != NULL) {
                *rtc_time = t;
            }
        }
    }

    if (bme_ready != NULL) {
        *bme_ready = bme_ok;
    }
    if (rtc_ready != NULL) {
        *rtc_ready = rtc_ok;
    }

    return make_result(ESP_OK, "BME280=%s RTC=%s",
                       bme_ok ? "ok" : "unavail",
                       rtc_ok ? "ok" : "unavail");
}

cmd_result_t cmd_read_env(float *temp, float *hum, float *pres)
{
    if (!s_initialized || s_cfg.read_env == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "env sensor not available");
    }
    measurement_t m;
    esp_err_t err = s_cfg.read_env(&m);
    if (err != ESP_OK) {
        return make_result(err, "env read failed: %s", esp_err_to_name(err));
    }
    if (temp != NULL) *temp = m.temperature_c;
    if (hum != NULL)  *hum = m.humidity_percent;
    if (pres != NULL) *pres = m.pressure_pa;
    return make_result(ESP_OK, "T=%.2fC H=%.1f%% P=%.0fPa",
                       m.temperature_c, m.humidity_percent, m.pressure_pa);
}

cmd_result_t cmd_read_power(power_reading_t *out)
{
    if (!s_initialized || s_cfg.read_power == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "power monitor not available");
    }
    power_reading_t p;
    esp_err_t err = s_cfg.read_power(&p);
    if (err != ESP_OK) {
        return make_result(err, "power read failed: %s", esp_err_to_name(err));
    }
    if (out != NULL) *out = p;
    return make_result(ESP_OK, "Vbat=%umV Vin=%umV Vsys=%umV Iin=%umA Icc=%umA",
                       p.battery_mv, p.input_mv, p.system_mv, p.input_ma, p.charge_ma);
}

/* Return current UTC milliseconds since epoch, sourced from the IDF's internal
 * clock (settimeofday'd from the RTC at boot — see components/pcf2131tfy_rtc).
 * Cheap: one syscall, no I2C transactions. */
static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

/* ── Publish power gate (Phase 1) ─────────────────────────────────────────
 * Only drain the MQTT backlog while on external power. Keyed on VIN-present
 * (input voltage / charger VIN status) rather than input current: Iin has a
 * ~13 mA ADC step and reads near-zero when the battery is full even in full
 * sun, so it under-reports available power. The charger reading is cached
 * (PUB_GATE_EVAL_INTERVAL_MS) so the drain loop's frequent re-checks stay cheap,
 * and the gate is debounced with asymmetric dwell times so a reading hovering
 * at the threshold can't toggle publishing on/off rapidly. */
#define PUB_GATE_EVAL_INTERVAL_MS   5000    /* re-read the charger at most this often */
#define PUB_GATE_ON_DWELL_MS       15000    /* power present this long ⇒ open the gate  */
#define PUB_GATE_OFF_DWELL_MS      60000    /* power absent this long  ⇒ close the gate */

static bool    s_pub_gate_open    = false;  /* debounced gate state (closed at boot) */
static bool    s_pub_gate_cand    = false;  /* last instantaneous reading vs. gate */
static int64_t s_pub_gate_cand_ms = 0;      /* when cand first differed from the gate */
static int64_t s_pub_gate_eval_ms = 0;      /* last charger evaluation (0 = never) */

/* Last known battery voltage, latched on every successful charger read (the
 * power gate and status report both feed it). 0 = never read; the envelope's
 * `device_battery` field is omitted in that case. */
static uint32_t s_last_batt_mv = 0;

bool device_commands_publish_power_ok(void)
{
    /* No power monitor wired in (dev board / absent charger): never gate, so
     * publishing behaves exactly as before this feature existed. */
    if (!s_initialized || s_cfg.read_power == NULL) {
        return true;
    }

    const int64_t now = now_ms();
    if (s_pub_gate_eval_ms != 0 &&
        (now - s_pub_gate_eval_ms) < PUB_GATE_EVAL_INTERVAL_MS) {
        return s_pub_gate_open;   /* cached between evaluations */
    }
    s_pub_gate_eval_ms = now;

    power_reading_t p;
    if (s_cfg.read_power(&p) != ESP_OK) {
        /* Transient I2C/ADC failure: hold the last decision rather than flap. */
        return s_pub_gate_open;
    }
    s_last_batt_mv = p.battery_mv;
    const bool present = p.input_present;

    if (present == s_pub_gate_open) {
        s_pub_gate_cand = present;            /* steady — clear any pending change */
    } else if (present != s_pub_gate_cand) {
        s_pub_gate_cand    = present;         /* new candidate — start the dwell timer */
        s_pub_gate_cand_ms = now;
    } else {
        const int64_t dwell = present ? PUB_GATE_ON_DWELL_MS : PUB_GATE_OFF_DWELL_MS;
        if ((now - s_pub_gate_cand_ms) >= dwell) {
            s_pub_gate_open = present;        /* debounce satisfied — flip the gate */
            ESP_LOGI(TAG, "publish gate %s (Vin=%umV Ibat=%umA)",
                     present ? "OPEN (external power)" : "CLOSED (on battery)",
                     p.input_mv, p.charge_ma);
        }
    }
    return s_pub_gate_open;
}

cmd_result_t cmd_record_env(int64_t *out_measure_id, measurement_t *out_reading)
{
    if (!s_initialized || s_cfg.read_env == NULL ||
        s_cfg.store_event == NULL || s_cfg.next_id == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "env/persistence not available");
    }

    int64_t start_ms = now_ms();
    measurement_t m;
    esp_err_t err = s_cfg.read_env(&m);
    int64_t end_ms = now_ms();
    if (err != ESP_OK) {
        return make_result(err, "env read failed: %s", esp_err_to_name(err));
    }
    if (out_reading) *out_reading = m;

    int64_t mid = 0;
    if ((err = s_cfg.next_id(&mid)) != ESP_OK) {
        return make_result(err, "next_id failed: %s", esp_err_to_name(err));
    }

    /* One event: T/H/P together in the payload. channel "" = onboard sensor. */
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.1f}",
             m.temperature_c, m.humidity_percent, m.pressure_pa);

    measurement_event_desc_t d = {
        .measure_id   = mid,
        .tag          = MEASUREMENT_TAG_MEASUREMENT,
        .cmd_raw      = "device.bme280",
        .start_ms     = start_ms,
        .end_ms       = end_ms,
        .payload_json = payload,
    };
    err = s_cfg.store_event(&d);
    if (err != ESP_OK) {
        return make_result(err, "store failed: %s", esp_err_to_name(err));
    }
    notify_sync();   /* wake the publisher */

    if (out_measure_id) *out_measure_id = mid;
    return make_result(ESP_OK,
                       "recorded env id=%lld: T=%.2fC H=%.1f%% P=%.0fPa",
                       (long long)mid, m.temperature_c,
                       m.humidity_percent, m.pressure_pa);
}

cmd_result_t cmd_log(const char *msg)
{
    if (msg == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "msg is NULL");
    }
    ESP_LOGI(TAG, "%s", msg);
    return make_result(ESP_OK, "logged");
}

cmd_result_t cmd_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    return make_result(ESP_OK, "slept %lu ms", (unsigned long)ms);
}

cmd_result_t cmd_sd_ready(bool *out_ready)
{
    if (out_ready == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "out_ready is NULL");
    }
    if (!s_initialized || s_cfg.sd_ready == NULL) {
        *out_ready = false;
        return make_result(ESP_ERR_NOT_SUPPORTED, "SD port not wired");
    }
    *out_ready = s_cfg.sd_ready();
    return make_result(ESP_OK, "SD: %s", *out_ready ? "ready" : "out");
}

cmd_result_t cmd_next_measure_id(int64_t *out_id)
{
    if (!s_initialized || s_cfg.next_id == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "persistence not available");
    }
    esp_err_t err = s_cfg.next_id(out_id);
    if (err != ESP_OK) {
        return make_result(err, "next_id failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "next_id: %lld", (long long)*out_id);
}

cmd_result_t cmd_mqtt_status(void)
{
    if (!s_initialized || s_cfg.message_is_connected == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "MQTT not available");
    }
    bool connected = s_cfg.message_is_connected();
    return make_result(ESP_OK, "MQTT: %s", connected ? "connected" : "disconnected");
}

cmd_result_t cmd_db_status(bool *available, int64_t *total,
                           int64_t *pending, int64_t *next_id)
{
    if (!s_initialized || s_cfg.db_stats == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "persistence not available");
    }
    bool    avail = false;
    int64_t tot = 0, pend = 0, nid = 0;
    esp_err_t err = s_cfg.db_stats(&avail, &tot, &pend, &nid);
    if (err != ESP_OK) {
        return make_result(err, "db stats failed: %s", esp_err_to_name(err));
    }
    if (available) *available = avail;
    if (total)     *total     = tot;
    if (pending)   *pending   = pend;
    if (next_id)   *next_id   = nid;
    return make_result(ESP_OK, "DB=%s total=%lld pending=%lld next_id=%lld",
                       avail ? "online" : "offline",
                       (long long)tot, (long long)pend, (long long)nid);
}

/* ── Event store + publish (one row/event; one message per measure_id) ── */

cmd_result_t cmd_store_event(const measurement_event_desc_t *desc)
{
    if (!s_initialized || s_cfg.store_event == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "event storage not available (no SD?)");
    }
    if (desc == NULL || desc->payload_json == NULL ||
        desc->tag == NULL || desc->tag[0] == '\0') {
        return make_result(ESP_ERR_INVALID_ARG, "store_event: null arg");
    }
    esp_err_t err = s_cfg.store_event(desc);
    if (err != ESP_OK) {
        return make_result(err, "store_event(%s) failed: %s",
                           desc->cmd_raw ? desc->cmd_raw : "", esp_err_to_name(err));
    }
    notify_sync();   /* wake the publisher */
    return make_result(ESP_OK, "stored event id=%lld (%s)",
                       (long long)desc->measure_id, desc->cmd_raw ? desc->cmd_raw : "");
}

/* Publish the next pending event as one MQTT message (one measure_id = one
 * message). Keeps the cloud's `sample:[…]` wrapper; the event's quantities are
 * nested under `data`. */
static dc_stuck_slot_t *stuck_slot(int64_t mid, bool create)
{
    dc_stuck_slot_t *free_slot = NULL;
    for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
        if (s_stuck[i].measure_id == mid) return &s_stuck[i];
        if (free_slot == NULL && s_stuck[i].measure_id == 0) free_slot = &s_stuck[i];
    }
    if (create && free_slot != NULL) {
        free_slot->measure_id = mid;
        return free_slot;
    }
    return NULL;
}

static void clear_publish_stuck(int64_t mid)
{
    dc_stuck_slot_t *slot = stuck_slot(mid, false);
    if (slot != NULL) memset(slot, 0, sizeof *slot);
}

/* Count only record-local heap/publish failures, keyed independently for every
 * measure_id in the window. A later record's success clears only its own entry.
 * Quarantine still fails closed for a non-frontier id; the capped counter stays
 * live and retries once that record becomes the cursor frontier. */
static unsigned note_publish_stuck(int64_t mid)
{
    dc_stuck_slot_t *slot = stuck_slot(mid, true);
    if (slot == NULL) {
        ESP_LOGE(TAG, "stuck-counter table full for id=%lld — holding record", (long long)mid);
        return 0;
    }
    if (slot->failures < UINT8_MAX) slot->failures++;
    if (slot->failures < DC_PUBLISH_STUCK_MAX || s_cfg.quarantine_event == NULL) {
        return slot->failures;
    }

    esp_err_t err = s_cfg.quarantine_event(mid);
    if (err == ESP_OK) {
        ESP_LOGE(TAG, "event id=%lld quarantined after %d consecutive failed publish "
                      "attempts — archived on SD, drain unblocked",
                 (long long)mid, (int)slot->failures);
        memset(slot, 0, sizeof *slot);
    } else {
        ESP_LOGW(TAG, "quarantine of stuck event id=%lld failed (%s) — will retry",
                 (long long)mid, esp_err_to_name(err));
        slot->failures = DC_PUBLISH_STUCK_MAX - 1U;
    }
    return slot->failures;
}

cmd_result_t cmd_mqtt_publish_next_event(void)
{
    if (!s_initialized || s_cfg.publish == NULL ||
        s_cfg.claim_next_event == NULL || s_cfg.mark_event_pending == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "MQTT or persistence not available");
    }
    /* Don't even claim an event if the broker isn't connected — Wi-Fi/MQTT
     * may be down and we'd just shove bytes at a dead pipe (the esp-mqtt
     * client would reject/drop them). Keeps events PENDING for the
     * next cycle and lets sync_runner_drain exit silently. */
    if (s_cfg.message_is_connected != NULL && !s_cfg.message_is_connected()) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "MQTT not connected");
    }
    /* Power gate lives solely in sync_runner_is_allowed() now — sync_runner is
     * the only caller of this function (Lua can no longer publish directly), so
     * one check there is sufficient and unbypassable.  Count admission is checked
     * before touching FATFS; byte admission is exact once the envelope exists. */
    size_t active_slots = 0, outstanding_bytes = 0;
    portENTER_CRITICAL(&s_inflight_mtx);
    inflight_usage_locked(&active_slots, &outstanding_bytes);
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (active_slots >= PUBLISH_WINDOW_SLOTS || outstanding_bytes >= PUBLISH_WINDOW_BYTES) {
        return make_result(ESP_ERR_INVALID_STATE,
                           "publish window full (%u/%u slots, %u/%u bytes)",
                           (unsigned)active_slots, (unsigned)PUBLISH_WINDOW_SLOTS,
                           (unsigned)outstanding_bytes, (unsigned)PUBLISH_WINDOW_BYTES);
    }

    measurement_event_t e;
    esp_err_t err = s_cfg.claim_next_event(&e);
    if (err == ESP_ERR_NOT_FOUND) {
        return make_result(ESP_ERR_NOT_FOUND, "no pending measurements");
    }
    if (err != ESP_OK) {
        return make_result(err, "claim_next_event failed: %s", esp_err_to_name(err));
    }

    /* Build the MQTT envelope (schema v2) as a string, splicing the already-valid
     * payload (and metadata) JSON in verbatim — no cJSON_Parse round-trip. The old
     * parse→tree→print path needed ~4× the payload in heap (a node per number),
     * which OOMs on this tight heap for multi-array runs, and scaled with point
     * count. This needs one buffer ≈ payload size + a fixed envelope. The
     * channel/device/tag/cmd_raw/config strings are controlled (firmware-built or
     * provisioned, sanitized by the event log) so they are quoted directly;
     * payload_json/metadata_json are already valid JSON.
     *
     * Envelope `timestamp` = the MEASUREMENT time (startTicks): the cloud
     * pipeline aliases it to measurement_time_utc, so battery-queued events must
     * carry their capture time, not the publish time. Publish time goes into the
     * sample as `published`. */
    char meas_ts[32], pub_ts[32], meas_local[40];
    {
        struct tm tm_info;
        time_t ts = (time_t)(e.start_ticks_ms / 1000);
        gmtime_r(&ts, &tm_info);
        strftime(meas_ts, sizeof(meas_ts), "%Y-%m-%dT%H:%M:%SZ", &tm_info);
        ts = time(NULL);
        gmtime_r(&ts, &tm_info);
        strftime(pub_ts, sizeof(pub_ts), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

        /* timestamp_local: the SAME instant as `timestamp` (measurement start),
         * rendered in the device's local zone with an explicit ±HH:MM suffix so it
         * is unambiguous ISO-8601 (not bare wall time) and encodes DST. The offset
         * is DST-resolved for that instant, matching the scheduler's frame — so a
         * battery-queued winter event localizes with the winter offset. Use the
         * PURE timezone_utc_offset_seconds(); timezone_localize() must NOT be called
         * here — it mutates time_sync's stored offset (a scheduler-only side effect). */
        int32_t off = timezone_utc_offset_seconds((int64_t)(e.start_ticks_ms / 1000));
        time_t loc = (time_t)(e.start_ticks_ms / 1000) + off;
        gmtime_r(&loc, &tm_info);
        size_t k = strftime(meas_local, sizeof(meas_local), "%Y-%m-%dT%H:%M:%S", &tm_info);
        int32_t ao = off < 0 ? -off : off;
        snprintf(meas_local + k, sizeof(meas_local) - k, "%c%02d:%02d",
                 off < 0 ? '-' : '+', (int)(ao / 3600), (int)((ao % 3600) / 60));
    }

    /* "" → JSON null for the optional provenance strings. */
    char chanbuf[16], devbuf[32];
    if (e.channel[0] != '\0') snprintf(chanbuf, sizeof(chanbuf), "\"%s\"", e.channel);
    else                      strcpy(chanbuf, "null");
    if (e.device[0] != '\0')  snprintf(devbuf, sizeof(devbuf), "\"%s\"", e.device);
    else                      strcpy(devbuf, "null");
    /* cmd_raw is variable-length (a full "arrun …" can be ~520 B) → heap-quoted;
     * NULL falls back to the JSON null literal in the splice below. */
    char *cmdbuf = NULL;
    if (e.cmd_raw != NULL && e.cmd_raw[0] != '\0') {
        size_t cn = strlen(e.cmd_raw);
        cmdbuf = malloc(cn + 3);
        if (cmdbuf == NULL) {
            s_cfg.mark_event_pending(e.measure_id);
            measurement_event_free(&e);
            return make_result(ESP_ERR_NO_MEM, "cmd_raw buf alloc failed (%u B)", (unsigned)(cn + 3));
        }
        cmdbuf[0] = '"';
        memcpy(cmdbuf + 1, e.cmd_raw, cn);
        cmdbuf[cn + 1] = '"';
        cmdbuf[cn + 2] = '\0';
    }
    const char *cmdfield = cmdbuf ? cmdbuf : "null";

    /* Optional envelope fields — empty string when absent. */
    char battpart[48] = "";
    if (s_last_batt_mv != 0) {
        snprintf(battpart, sizeof(battpart), "\"device_battery\":%.3f,",
                 (double)s_last_batt_mv / 1000.0);
    }
    char tzpart[96] = "";
    if (s_cfg.timezone != NULL && s_cfg.timezone[0] != '\0') {
        snprintf(tzpart, sizeof(tzpart), "\"timezone\":\"%s\",", s_cfg.timezone);
    }

    const char *meta = e.metadata_json;          /* already a JSON object, or NULL */
    const char *fw   = s_cfg.device_firmware  ? s_cfg.device_firmware  : "";
    const char *dn   = s_cfg.device_name      ? s_cfg.device_name      : "";
    const char *dv   = s_cfg.device_version   ? s_cfg.device_version   : "";

    /* Size the exact serialized envelope before any large allocation. This lets
     * a lowered publish cap identify a valid 64-KiB stored record and archive it
     * without first tripping the ordinary internal/DMA heap gate. */
    int n = snprintf(NULL, 0, DC_EVENT_ENVELOPE_FMT,
        (long long)e.measure_id, (long long)e.start_ticks_ms, (long long)e.end_ticks_ms,
        meas_local, pub_ts, chanbuf, devbuf, cmdfield, e.tag,
        meta ? meta : "null", e.payload_json,
        meas_ts, battpart, tzpart,
        s_mac_str, dn, dv, fw);
    if (n < 0) {
        free(cmdbuf);
        s_cfg.mark_event_pending(e.measure_id);
        measurement_event_free(&e);
        return make_result(ESP_FAIL, "envelope sizing failed");
    }
    size_t payload_len = (size_t)n;
    size_t cap = payload_len + 1U;

    /* This policy branch is intended for builds that deliberately lower the
     * publish ceiling (for example 16 KiB) below the normal store capability.
     * The derived default includes envelope headroom above every stored record. */
    if (payload_len > AMBYTE_PUBLISH_MAX_BYTES) {
        int64_t mid = e.measure_id;
        free(cmdbuf);
        esp_err_t pending_err = s_cfg.mark_event_pending(mid);
        if (s_oversize_warned_id != mid) {
            ESP_LOGW(TAG, "event id=%lld envelope %u B exceeds AMBYTE_PUBLISH_MAX_BYTES=%u — preserving in quarantine sidecar",
                     (long long)mid, (unsigned)payload_len,
                     (unsigned)AMBYTE_PUBLISH_MAX_BYTES);
            s_oversize_warned_id = mid;
        }
        if (pending_err != ESP_OK) {
            measurement_event_free(&e);
            return make_result(pending_err,
                               "publish-cap event id=%lld could not revert claim: %s",
                               (long long)mid, esp_err_to_name(pending_err));
        }

        esp_err_t quarantine_err = s_cfg.quarantine_event != NULL
            ? s_cfg.quarantine_event(mid) : ESP_ERR_NOT_SUPPORTED;
        measurement_event_free(&e);
        if (quarantine_err == ESP_OK) {
            s_oversize_skipped++;
            clear_publish_stuck(mid);
            return make_result(ESP_OK,
                               "publish-cap skip id=%lld (%u>%u B): archived on SD (oversize_skipped=%u)",
                               (long long)mid, (unsigned)payload_len,
                               (unsigned)AMBYTE_PUBLISH_MAX_BYTES,
                               (unsigned)s_oversize_skipped);
        }
        return make_result(quarantine_err,
                           "publish-cap event id=%lld waiting for frontier/archive: %s",
                           (long long)mid, esp_err_to_name(quarantine_err));
    }

    /* Window-aware heap gate. MALLOC_CAP_8BIT totals include PSRAM and therefore
     * cannot prove that lwIP/Wi-Fi/TLS allocations fit. Charge the already queued
     * envelopes plus this allocation and fixed TLS headroom against BOTH largest
     * internal-DRAM and DMA-capable blocks. Large envelopes get one Lua-GC/settle
     * retry; a failure reverts only this event-log slot. */
    size_t heap_need = outstanding_bytes + cap + DC_PUBLISH_HEAP_HEADROOM;
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    bool heap_tight = largest_internal < heap_need || largest_dma < heap_need;
    if (heap_tight && cap > DC_LARGE_PUBLISH_BYTES) {
        if (s_cfg.request_gc != NULL) s_cfg.request_gc();
        vTaskDelay(pdMS_TO_TICKS(DC_PUBLISH_SETTLE_MS));
        portENTER_CRITICAL(&s_inflight_mtx);
        inflight_usage_locked(NULL, &outstanding_bytes);
        portEXIT_CRITICAL(&s_inflight_mtx);
        heap_need = outstanding_bytes + cap + DC_PUBLISH_HEAP_HEADROOM;
        largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        heap_tight = largest_internal < heap_need || largest_dma < heap_need;
    }
    if (heap_tight) {
        free(cmdbuf);
        s_cfg.mark_event_pending(e.measure_id);
        int64_t mid = e.measure_id;
        measurement_event_free(&e);
        /* If earlier envelopes are what make the pool tight, this is ordinary
         * window backpressure, not evidence that `mid` is poisonous. */
        unsigned attempt = outstanding_bytes == 0 ? note_publish_stuck(mid) : 0;
        return make_result(ESP_ERR_NO_MEM,
                           "publish deferred (window heap): id=%lld need=%uB int_largest=%u dma_largest=%u (attempt %u/%u)",
                           (long long)mid, (unsigned)heap_need,
                           (unsigned)largest_internal, (unsigned)largest_dma,
                           attempt, (unsigned)DC_PUBLISH_STUCK_MAX);
    }

    char *payload = malloc(cap);
    if (payload == NULL) {
        free(cmdbuf);
        s_cfg.mark_event_pending(e.measure_id);
        measurement_event_free(&e);
        return make_result(ESP_ERR_NO_MEM, "envelope alloc failed (%u B)", (unsigned)cap);
    }

    n = snprintf(payload, cap, DC_EVENT_ENVELOPE_FMT,
        (long long)e.measure_id, (long long)e.start_ticks_ms, (long long)e.end_ticks_ms,
        meas_local, pub_ts, chanbuf, devbuf, cmdfield, e.tag,
        meta ? meta : "null", e.payload_json,
        meas_ts, battpart, tzpart,
        s_mac_str, dn, dv, fw);
    free(cmdbuf);

    if (n < 0 || (size_t)n >= cap) {
        free(payload);
        s_cfg.mark_event_pending(e.measure_id);
        measurement_event_free(&e);
        return make_result(ESP_ERR_NO_MEM, "envelope build failed (n=%d cap=%u)",
                           n, (unsigned)cap);
    }

    /* Exact MQTT-window admission uses the serialized envelope length, not the
     * raw event-log line. If 15 small envelopes leave less room than this one,
     * revert this ONE claim and wait for completions; the event-log FIFO returns
     * it before admitting a newer offset, so the window cannot wedge or skip.
     * An empty MQTT window deliberately admits one envelope even above the
     * nominal byte budget; it serializes the link just like event_log's lone
     * maximum record. Once any envelope is outstanding, the budget is strict. */
    portENTER_CRITICAL(&s_inflight_mtx);
    inflight_usage_locked(&active_slots, &outstanding_bytes);
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (active_slots >= PUBLISH_WINDOW_SLOTS ||
        (outstanding_bytes > 0 &&
         outstanding_bytes + payload_len > PUBLISH_WINDOW_BYTES)) {
        int64_t mid = e.measure_id;
        free(payload);
        s_cfg.mark_event_pending(mid);
        measurement_event_free(&e);
        return make_result(ESP_ERR_INVALID_STATE,
                           "publish window byte/slot budget deferred id=%lld (%u/%u slots, %u+%u/%u bytes)",
                           (long long)mid, (unsigned)active_slots,
                           (unsigned)PUBLISH_WINDOW_SLOTS, (unsigned)outstanding_bytes,
                           (unsigned)payload_len, (unsigned)PUBLISH_WINDOW_BYTES);
    }

    /* The envelope now holds its own copy of the event's JSON, so free the claimed
     * event's heap strings (up to the 64-KiB record cap: payload + metadata + cmd)
     * BEFORE the publish. Otherwise they sit alongside esp-mqtt's outbox copy and
     * the mbedTLS write buffer. Only the heap pointers are
     * freed/nulled; e.measure_id/tag/channel (scalar + fixed arrays) stay valid for
     * the logging, latch, and return below, and the later measurement_event_free
     * calls become safe no-ops. */
    measurement_event_free(&e);

    if (payload_len >= MQTT_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "event payload %u bytes exceeds %u; may be rejected",
                 (unsigned)payload_len, (unsigned)MQTT_PAYLOAD_MAX);
    }
    char topic[MQTT_TOPIC_MAX];
    snprintf(topic, sizeof(topic), "%s/1234", s_cfg.topic_root ? s_cfg.topic_root : "");

    ESP_LOGI(TAG, "publish event -> %s (id=%lld, tag=%s, ch=%s, %u bytes)",
             topic, (long long)e.measure_id, e.tag,
             e.channel[0] ? e.channel : "-", (unsigned)payload_len);

    /* Heap probe for large payloads (arrun traces can approach 64 KiB): QoS1
     * retains an outbox copy while TLS writes sequential fragments. Large copies
     * follow the PSRAM malloc policy, but capability-specific largest blocks are
     * still logged to diagnose internal/DMA starvation. */
    if (payload_len > 2048) {
        ESP_LOGW(TAG, "large publish %u B (id=%lld): heap free=%u largest=%u",
                 (unsigned)payload_len, (long long)e.measure_id,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    /* Reserve the correlation token BEFORE publish. esp_mqtt_client_publish()
     * writes this QoS-1 packet synchronously on the drain task, so sixteen
     * back-to-back calls genuinely reach the wire without waiting for PUBACKs.
     * It then marks each outbox item TRANSMITTED, which keeps ticket 01's 10 s
     * message_retransmit_timeout policy effective. The API mutex unlocks just
     * before return, hence the early-ACK parks still close the cross-core gap. */
    int slot_idx = -1;
    int64_t latched_at_ms = mono_ms();
    portENTER_CRITICAL(&s_inflight_mtx);
    slot_idx = inflight_find_free_locked();
    if (slot_idx >= 0) {
        early_acks_clear_locked();
        s_inflight[slot_idx] = (dc_inflight_slot_t) {
            .used           = true,
            .measure_id     = e.measure_id,
            .msg_id         = -1,
            .since_ms       = latched_at_ms,
            .envelope_bytes = payload_len,
        };
    }
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (slot_idx < 0) {
        int64_t mid = e.measure_id;
        free(payload);
        s_cfg.mark_event_pending(mid);
        return make_result(ESP_ERR_INVALID_STATE, "publish window filled before send id=%lld",
                           (long long)mid);
    }

    int msg_id = 0;
    err = s_cfg.publish(topic, payload, payload_len, &msg_id);
    free(payload);
    if (err != ESP_OK) {
        bool owned_reservation = false;
        portENTER_CRITICAL(&s_inflight_mtx);
        if (s_inflight[slot_idx].used &&
            s_inflight[slot_idx].measure_id == e.measure_id &&
            s_inflight[slot_idx].msg_id < 0) {
            inflight_clear_locked((size_t)slot_idx);
            early_acks_clear_locked();
            owned_reservation = true;
        }
        portEXIT_CRITICAL(&s_inflight_mtx);

        ESP_LOGW(TAG, "publish failed id=%lld (%u B): int_largest=%u dma_largest=%u",
                 (long long)e.measure_id, (unsigned)payload_len,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        /* publish() aborts the connection on a synchronous write failure. If its
         * nested disconnect callback detached the reservation, that queued
         * completion owns the PENDING transition. Otherwise this sync-runner
         * call still owns the claim and reverts it directly. */
        if (owned_reservation) s_cfg.mark_event_pending(e.measure_id);
        /* The stuck counter quarantines poison records, not healthy records caught
         * in a network outage. The transport reports INVALID_STATE when the
         * connection vanished before/during publish; it may also drop between the call
         * and this check. Only a failure while still connected is record-local. */
        bool transport_refused = err == ESP_ERR_INVALID_STATE ||
            (s_cfg.message_is_connected != NULL && !s_cfg.message_is_connected());
        if (!transport_refused) {
            note_publish_stuck(e.measure_id);   /* live-link per-record failure */
        }
        measurement_event_free(&e);
        return make_result(err, "event publish failed: %s", esp_err_to_name(err));
    }

    /* Finalize the reservation. If the PUBACK was already parked, detach the
     * slot into the normal completion queue now. Otherwise publishing proceeds
     * with the concrete msg_id installed. */
    bool reservation_present = false;
    bool early_matched = false;
    dc_inflight_slot_t detached = {0};
    dc_ack_completion_t early_completion = {
        .measure_id = e.measure_id,
        .msg_id = msg_id,
        .status = ESP_OK,
        .kind = DC_ACK_PUBACK,
    };
    int64_t finalized_at_ms = mono_ms();
    portENTER_CRITICAL(&s_inflight_mtx);
    if (s_inflight[slot_idx].used &&
        s_inflight[slot_idx].measure_id == e.measure_id &&
        s_inflight[slot_idx].msg_id < 0) {
        reservation_present = true;
        s_inflight[slot_idx].msg_id = msg_id;
        for (size_t i = 0; i < s_early_ack_count; i++) {
            if (s_early_acks[i].msg_id == msg_id) {
                detached = s_inflight[slot_idx];
                early_completion.status = s_early_acks[i].status;
                early_completion.kind = s_early_acks[i].status == ESP_OK
                    ? DC_ACK_PUBACK : DC_ACK_PUBLISH_ERROR;
                inflight_clear_locked((size_t)slot_idx);
                if (early_completion.status == ESP_OK) s_last_publish_ok_ms = finalized_at_ms;
                early_matched = true;
                break;
            }
        }
        early_acks_clear_locked();
    }
    portEXIT_CRITICAL(&s_inflight_mtx);

    int64_t mid = e.measure_id;
    measurement_event_free(&e);
    /* A positive msg_id is a successful wire write even if its nested
     * disconnect callback already detached the reservation. Clear only this
     * record's poison streak before the race-owned early return (review m8). */
    clear_publish_stuck(mid);  /* B success cannot erase A's streak. */
    if (!reservation_present) {
        return make_result(ESP_ERR_INVALID_STATE,
                           "publish id=%lld raced disconnect; pending completion owns replay",
                           (long long)mid);
    }

    if (early_matched && !enqueue_ack_completion(&early_completion)) {
        detached.msg_id = msg_id;
        portENTER_CRITICAL(&s_inflight_mtx);
        int restore_idx = !s_inflight[slot_idx].used
            ? slot_idx : inflight_find_free_locked();
        if (restore_idx >= 0) s_inflight[restore_idx] = detached;
        portEXIT_CRITICAL(&s_inflight_mtx);
    }
    return make_result(ESP_OK, "published event id=%lld msg_id=%d", (long long)mid, msg_id);
}

/* ── Status report + heartbeat publish ─────────────────────────────────── */

cmd_result_t cmd_status_report(device_status_snapshot_t *out)
{
    if (out == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "status_report: null out");
    }
    memset(out, 0, sizeof(*out));

    out->wifi_connected = wifi_manager_is_connected();
    (void)wifi_manager_is_provisioned(&out->provisioned);

    int64_t total = 0, next_id = 0;
    (void)cmd_db_status(&out->db_online, &total, &out->pending, &next_id);

    if (s_cfg.read_power != NULL && s_cfg.read_power(&out->power) == ESP_OK) {
        out->power_valid = true;
        s_last_batt_mv = out->power.battery_mv;
    }
    out->publish_gate_open = device_commands_publish_power_ok();

    /* On-board BME280 environment, so every heartbeat carries T/H/P even when
     * main.lua isn't measuring (broken/missing script, or power-gated). Same
     * keys/format as the device.bme280 measurement event (cmd_record_env). */
    if (s_cfg.read_env != NULL) {
        measurement_t m;
        if (s_cfg.read_env(&m) == ESP_OK) {
            out->env_valid         = true;
            out->temperature_c     = m.temperature_c;
            out->humidity_percent  = m.humidity_percent;
            out->pressure_pa       = m.pressure_pa;
        }
    }

    return make_result(ESP_OK, "status report");
}

/* Build + store one STATUS heartbeat event from the live status snapshot
 * (tag STATUS, onboard provenance — channel/cmd_raw null). Owned by the
 * sync_runner heartbeat (payload-v2 Phase 4): status reporting must survive a
 * missing/crashed main.lua, so it does NOT live in the script. Payload keys
 * match the old Lua status_report table for analysis continuity; power fields
 * are omitted when the charger read fails, and BME280 T/H/P fields are appended
 * when the env read succeeds. Does NOT notify the sync runner — the caller IS
 * the sync runner. */
cmd_result_t cmd_store_status_event(void)
{
    if (!s_initialized || s_cfg.store_event == NULL || s_cfg.next_id == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "persistence not available");
    }

    device_status_snapshot_t s;
    cmd_result_t r = cmd_status_report(&s);
    if (r.status != ESP_OK) {
        return r;
    }

    uint32_t mqtt_connects = 0;
    int64_t conn_age_s = -1;
    char last_disc_reason[24] = {0};
    if (s_cfg.connection_stats != NULL) {
        s_cfg.connection_stats(&mqtt_connects, &conn_age_s,
                               last_disc_reason, sizeof last_disc_reason);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char app_version[32];
    status_copy_json_safe(app_version, sizeof app_version,
                          app != NULL ? app->version : "");
    bool wd_armed = s_cfg.watchdog_armed != NULL && s_cfg.watchdog_armed();
    char wd_reason[16] = {0};
    if (s_cfg.last_wd_reboot_reason != NULL) {
        (void)s_cfg.last_wd_reboot_reason(wd_reason, sizeof wd_reason);
    }
    const char *clock_source = "rtc";
    bool clock_suspect = false;
    clock_trust_get_status(&clock_source, &clock_suspect);
    script_identity_t script_identity = {0};
    bool script_identity_valid = s_cfg.read_script_identity != NULL &&
                                 s_cfg.read_script_identity(&script_identity) == ESP_OK;
    if (script_identity_valid) {
        status_copy_json_safe(script_identity.sha256, sizeof script_identity.sha256,
                              script_identity.sha256);
        status_copy_json_safe(script_identity.version, sizeof script_identity.version,
                              script_identity.version);
        status_copy_json_safe(script_identity.built_against_fw,
                              sizeof script_identity.built_against_fw,
                              script_identity.built_against_fw);
        status_copy_json_safe(script_identity.installed_on_fw,
                              sizeof script_identity.installed_on_fw,
                              script_identity.installed_on_fw);
    }

    /* Schema split (2026-07-28, Dominik): the sample's `data` object carries only
     * MEASUREMENTS (the BME280 environment readings); all device-health/info
     * fields live in the sample's `metadata` object, and `device` carries the
     * MAC. Platform charts device health from metadata, science from data.
     * The bounded base always fits. Optional script, SD, power, and per-channel
     * AMBIT blocks are appended transactionally below. */
    /* Worst-case budget at declared format maxima (%.3f of a pathological
     * negative mV reading renders 10 chars ×3; the u32 currents render 10
     * digits; every string block at its cap):
     *   base + SD + power + '}' + NUL         891 B
     *   script identity (digest+3 versions)   286 B
     *   AMBIT identity, 4 × 138 B             552 B   (host-measured)
     *                                       ------
     *                                        1729 B
     * The 2,048-B buffer leaves 319 B spare. On the bench (2 AMBITs, real
     * values) the ambit blocks total 232 B. DO NOT add fields against
     * typical-value headroom: budget against these maxima, or grow the buffer.
     * An overflow doesn't corrupt (status_append_optional drops the offending
     * block with a WARN) but silently costs that block on outlier readings.
     * This buffer lives on the wd-task frame — SYNC_WD_TASK_STACK covers its
     * growth from the original 896 B. Identity hashing uses a short-lived 1-KiB
     * heap buffer so the heartbeat task does not carry it on the stack. */
    char payload[2048];
    int n = snprintf(payload, sizeof(payload),
        "{\"wifi\":%s,\"provisioned\":%s,\"db_online\":%s,\"publish_gate\":%s,"
        "\"uptime_s\":%lld,\"psram_free_kb\":%u,\"psram_largest_kb\":%u,"
        "\"psram_size_kb\":%u,\"heap_dma_largest_kb\":%u,\"mqtt_reconnects\":%u,"
        "\"last_disc_reason\":\"%.23s\",\"conn_age_s\":%lld,\"pending\":%lld,"
        "\"last_wd_reboot_reason\":\"%.15s\",\"wd_armed\":%s,\"app_version\":\"%.31s\","
        "\"clock_src\":\"%.4s\",\"clock_suspect\":%s,"
        "\"heap_int_free_kb\":%u,\"heap_int_largest_kb\":%u",
        s.wifi_connected ? "true" : "false",
        s.provisioned ? "true" : "false",
        s.db_online ? "true" : "false",
        s.publish_gate_open ? "true" : "false",
        (long long)(esp_timer_get_time() / 1000000LL),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024),
        (unsigned)mqtt_connects, last_disc_reason,
        (long long)conn_age_s, (long long)s.pending,
        wd_reason, wd_armed ? "true" : "false", app_version,
        clock_source, clock_suspect ? "true" : "false",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    if (n < 0 || (size_t)n + 2 > sizeof(payload)) {
        return make_result(ESP_FAIL, "status base payload build failed");
    }
    size_t payload_len = (size_t)n;

    if (script_identity_valid) {
        (void)status_append_optional(payload, sizeof payload, &payload_len, "script",
            ",\"script_sha256\":\"%.64s\",\"script_version\":\"%.31s\","
            "\"script_built_against_fw\":\"%.31s\",\"script_installed_on_fw\":\"%.31s\","
            "\"script_metadata_verified\":%s",
            script_identity.sha256, script_identity.version,
            script_identity.built_against_fw, script_identity.installed_on_fw,
            script_identity.release_metadata_verified ? "true" : "false");
    }

    /* SD/persistence health — surfaces the audit's silent-loss counters so a
     * degrading card is visible before the cliff (free space, skipped/dropped
     * records, delivery high-water, io-lost). */
    if (s_cfg.sd_health != NULL) {
        bool sd_io_lost = false;
        uint64_t sd_free = 0;
        int64_t sd_skipped = 0, sd_dropped = 0, last_acked = 0;
        if (s_cfg.sd_health(&sd_io_lost, &sd_free, &sd_skipped, &sd_dropped, &last_acked) == ESP_OK) {
            (void)status_append_optional(payload, sizeof payload, &payload_len, "sd",
                ",\"sd_free_kb\":%llu,\"sd_skipped\":%lld,\"sd_dropped\":%lld,"
                "\"last_acked_id\":%lld,\"sd_io_lost\":%s",
                (unsigned long long)(sd_free / 1024),
                (long long)sd_skipped, (long long)sd_dropped, (long long)last_acked,
                sd_io_lost ? "true" : "false");
        }
    }
    if (s.power_valid) {
        (void)status_append_optional(payload, sizeof payload, &payload_len, "power",
            ",\"battery_v\":%.3f,\"input_v\":%.3f,\"system_v\":%.3f,"
            "\"input_ma\":%u,\"charge_ma\":%u,\"input_present\":%s,\"charge_status\":%u",
            (double)s.power.battery_mv / 1000.0,
            (double)s.power.input_mv / 1000.0,
            (double)s.power.system_mv / 1000.0,
            (unsigned)s.power.input_ma,
            (unsigned)s.power.charge_ma,
            s.power.input_present ? "true" : "false",
            (unsigned)s.power.charge_status);
    }

    /* Per-channel AMBIT identity (fleet inventory: which sensor + which fw is
     * on which channel). CACHE-ONLY — never triggers a UART fetch from the
     * heartbeat (a fetch is 2×5 s blocking + channel-mutex contention with
     * Lua); boot sync populates the cache. Absent/unread channels are simply
     * omitted. One transactional block per channel so a single outlier only
     * drops itself. ambit_name comes from the AMBIT's NVS = untrusted bytes;
     * event_log does NOT sanitize metadata_json, so it goes through
     * status_copy_json_safe (fw/id/cal are ambyte-formatted and safe). */
    for (uint8_t ch = 0; ch < 4; ch++) {
        ambit_device_info_t ai;
        if (!cmd_ambit_device_info_cached(ch, &ai)) {
            continue;
        }
        char safe_name[20];
        status_copy_json_safe(safe_name, sizeof safe_name, ai.ambit_name);
        char block[16];
        snprintf(block, sizeof block, "ambit%u", ch);
        (void)status_append_optional(payload, sizeof payload, &payload_len, block,
            ",\"ambit%u_fw\":\"%.15s\",\"ambit%u_hw\":%u,\"ambit%u_name\":\"%s\","
            "\"ambit%u_id\":\"%.17s\",\"ambit%u_cal\":\"%08lx\"",
            ch, ai.fw_version, ch, (unsigned)ai.hw_rev, ch, safe_name,
            ch, ai.device_id, ch, (unsigned long)ai.cal_version);
    }
    payload[payload_len++] = '}';
    payload[payload_len] = '\0';

    /* `data` = measurements only: the BME280 environment readings. Empty object
     * when the sensor read failed this cycle — data consumers never see device
     * health here. */
    char env_data[128];
    if (s.env_valid) {
        int en = snprintf(env_data, sizeof env_data,
            "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.1f}",
            s.temperature_c, s.humidity_percent, s.pressure_pa);
        if (en < 0 || (size_t)en >= sizeof env_data) {
            strcpy(env_data, "{}");
        }
    } else {
        strcpy(env_data, "{}");
    }

    int64_t mid = 0;
    esp_err_t err = s_cfg.next_id(&mid);
    if (err != ESP_OK) {
        return make_result(err, "next_id failed: %s", esp_err_to_name(err));
    }

    int64_t now = now_ms();
    measurement_event_desc_t d = {
        .measure_id    = mid,
        .device        = s_mac_str,          /* device = MAC (2026-07-28 ask) */
        .tag           = MEASUREMENT_TAG_STATUS,
        .start_ms      = now,
        .end_ms        = now,
        .metadata_json = payload,            /* device health/info */
        .payload_json  = env_data,           /* measurements only */
    };
    err = s_cfg.store_event(&d);
    if (err != ESP_OK) {
        return make_result(err, "status store failed: %s", esp_err_to_name(err));
    }
    return make_result(ESP_OK, "STATUS id=%lld gate=%s Vbat=%umV",
                       (long long)mid, s.publish_gate_open ? "OPEN" : "CLOSED",
                       (unsigned)(s.power_valid ? s.power.battery_mv : 0));
}

/* Last battery voltage latched from any successful charger read (power gate /
 * status report). 0 = never read. Probe for the status-LED blinker. */
uint32_t device_commands_last_battery_mv(void)
{
    return s_last_batt_mv;
}

cmd_result_t cmd_uart_stream_query(uint8_t channel, const char *cmd,
                                   const char *sentinel, uint32_t timeout_ms,
                                   char *out, size_t out_cap, size_t *out_len)
{
    if (!s_initialized || s_cfg.uart_stream_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART stream query not available");
    }
    if (cmd == NULL || out == NULL || out_len == NULL || out_cap < 2) {
        return make_result(ESP_ERR_INVALID_ARG, "stream_query: bad args");
    }
    sensor_transaction_begin();
    esp_err_t err = s_cfg.uart_stream_query(channel, cmd, "\n", sentinel,
                                            out, out_cap, out_len, timeout_ms);
    sensor_transaction_end();
    if (err == ESP_ERR_TIMEOUT) {
        return make_result(ESP_ERR_TIMEOUT, "stream timeout (no '%s')", sentinel ? sentinel : "");
    }
    if (err != ESP_OK) {
        return make_result(err, "stream_query ch%u failed: %s", channel, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "stream ch%u: %u bytes", channel, (unsigned)*out_len);
}

void device_commands_on_mqtt_disconnect(void)
{
    /* This callback runs on esp-mqtt/sys_evt stacks and can be nested inside the
     * sync-runner's synchronous publish() failure. A 16-element detach array
     * consumed ~960 B in the review build. Detach and enqueue ONE slot per lock
     * acquisition instead: queue order remains table order, no I/O occurs under
     * the portMUX, and this frame stays small on all three callers. */
    while (true) {
        size_t detached_idx = PUBLISH_WINDOW_SLOTS;
        dc_inflight_slot_t detached = {0};
        portENTER_CRITICAL(&s_inflight_mtx);
        for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
            if (!s_inflight[i].used) continue;
            detached_idx = i;
            detached = s_inflight[i];
            inflight_clear_locked(i);
            if (detached.msg_id < 0) early_acks_clear_locked();
            break;
        }
        portEXIT_CRITICAL(&s_inflight_mtx);
        if (detached_idx == PUBLISH_WINDOW_SLOTS) break;

        dc_ack_completion_t completion = {
            .measure_id = detached.measure_id,
            .msg_id     = detached.msg_id,
            .status     = ESP_FAIL,
            .kind       = DC_ACK_DISCONNECT,
        };
        if (!enqueue_ack_completion(&completion)) {
            portENTER_CRITICAL(&s_inflight_mtx);
            int restore_idx = !s_inflight[detached_idx].used
                ? (int)detached_idx : inflight_find_free_locked();
            if (restore_idx >= 0) s_inflight[restore_idx] = detached;
            portEXIT_CRITICAL(&s_inflight_mtx);
            /* Do not detach the restored slot again in a tight loop while the
             * queue is full. Its token remains fail-closed for the reaper. */
            break;
        }
    }
}

void device_commands_on_persistence_reset(void)
{
    /* event_log has discarded its volatile claim window after an SD reopen.
     * Atomically discard the peer correlation tokens, then reset stale queued
     * completions. The durable cursor will re-claim every unresolved record;
     * late callbacks find no msg-id match and are harmless duplicates. */
    portENTER_CRITICAL(&s_inflight_mtx);
    memset(s_inflight, 0, sizeof s_inflight);
    early_acks_clear_locked();
    s_ack_drops = 0;
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (s_ack_queue != NULL) xQueueReset(s_ack_queue);
    notify_sync();
}

void device_commands_abort_inflight(void)
{
    portENTER_CRITICAL(&s_inflight_mtx);
    memset(s_inflight, 0, sizeof s_inflight);
    early_acks_clear_locked();
    portEXIT_CRITICAL(&s_inflight_mtx);
    /* CLI task versus sync-runner consumer: xQueueReset may race a consumer that
     * already peeked/applied the head. That race is benign — its final receive
     * returns INVALID_STATE, the runner retries, and rewind still owns the cursor. */
    if (s_ack_queue != NULL) xQueueReset(s_ack_queue);
    /* Deliberately does NOT mark_event_pending: the caller (evlog rewind) has
     * already moved the persistence cursor, so the event is pending by position.
     * Drop deferred completions too; a late PUBACK for the abandoned msg_id is a
     * harmless no-op because the RAM latch and event_log claim are both cleared. */
}

bool device_commands_reap_stale_inflight(int64_t max_age_ms)
{
    bool    stale = false;
    int64_t mid   = -1;
    int64_t now   = mono_ms();
    int stale_idx = -1;
    dc_inflight_slot_t detached = {0};
    portENTER_CRITICAL(&s_inflight_mtx);
    for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
        if (s_inflight[i].used && (now - s_inflight[i].since_ms) >= max_age_ms) {
            stale = true;
            stale_idx = (int)i;
            detached = s_inflight[i];
            mid = detached.measure_id;
            inflight_clear_locked(i);
            if (detached.msg_id < 0) early_acks_clear_locked();
            break;  /* one slot per call; drain immediately loops and re-checks */
        }
    }
    portEXIT_CRITICAL(&s_inflight_mtx);

    if (stale) {
        ESP_LOGW(TAG, "reaped stale in-flight publish (id=%lld) — no PUBACK in %lld ms, will re-publish",
                 (long long)mid, (long long)max_age_ms);
        /* mid < 0 = an injected test slot (no real event): clear only. */
        if (mid >= 0 && s_cfg.mark_event_pending != NULL) {
            esp_err_t err = s_cfg.mark_event_pending(mid);
            if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_SUPPORTED) {
                /* event_log reset/offline is terminal for this volatile token.
                 * Restoring it would wedge the reaper forever; the durable
                 * cursor will replay the record when persistence is available. */
                ESP_LOGW(TAG, "discarding stale latch id=%lld after persistence reset (%s)",
                         (long long)mid, esp_err_to_name(err));
            } else if (err != ESP_OK) {
                /* Keep one retry token if persistence was temporarily busy;
                 * otherwise the claimed event-log slot could become orphaned. */
                portENTER_CRITICAL(&s_inflight_mtx);
                int restore_idx = !s_inflight[stale_idx].used
                    ? stale_idx : inflight_find_free_locked();
                if (restore_idx >= 0) s_inflight[restore_idx] = detached;
                portEXIT_CRITICAL(&s_inflight_mtx);
                return false;
            }
        }
    }
    return stale;
}

int64_t device_commands_ms_since_publish_ok(void)
{
    portENTER_CRITICAL(&s_inflight_mtx);
    int64_t t = s_last_publish_ok_ms;
    portEXIT_CRITICAL(&s_inflight_mtx);
    return mono_ms() - t;
}

void device_commands_inflight_status(int *msg_id, int64_t *measure_id, int64_t *age_ms)
{
    int     mi  = -1;
    int64_t me  = -1;
    int64_t age = 0;
    int64_t now = mono_ms();
    portENTER_CRITICAL(&s_inflight_mtx);
    for (size_t i = 0; i < PUBLISH_WINDOW_SLOTS; i++) {
        if (!s_inflight[i].used) continue;
        int64_t candidate_age = now - s_inflight[i].since_ms;
        if (me < 0 || candidate_age > age) {
            mi = s_inflight[i].msg_id;
            me = s_inflight[i].measure_id;
            age = candidate_age;
        }
    }
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (msg_id)     *msg_id     = mi;
    if (measure_id) *measure_id = me;
    if (age_ms)     *age_ms     = age;
}

void device_commands_window_status(size_t *slots, size_t *bytes)
{
    size_t n = 0, b = 0;
    portENTER_CRITICAL(&s_inflight_mtx);
    inflight_usage_locked(&n, &b);
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (slots != NULL) *slots = n;
    if (bytes != NULL) *bytes = b;
}

void device_commands_inject_stale_inflight(void)
{
    int64_t stale_since_ms = mono_ms() - (10 * 60 * 1000);
    bool inserted = false;
    portENTER_CRITICAL(&s_inflight_mtx);
    int idx = inflight_find_free_locked();
    if (idx >= 0) {
        s_inflight[idx] = (dc_inflight_slot_t) {
            .used       = true,
            .measure_id = -1,          /* no real event — reaper touches no data */
            .msg_id     = 0x7FFFFFFF,  /* bogus id no PUBACK will ever match */
            .since_ms   = stale_since_ms,
        };
        inserted = true;
    }
    portEXIT_CRITICAL(&s_inflight_mtx);
    if (inserted) {
        ESP_LOGW(TAG, "injected fake stale in-flight slot (test hook) — expect a reap on next drain");
    } else {
        ESP_LOGW(TAG, "cannot inject stale slot: publish window is full");
    }
}

/* ── Measurement activity + narrow publish hold ─────────────────────────
 * Measurement activity retains its whole-cycle PM/telemetry semantics. The
 * sync runner normally consults publish_hold instead; the legacy escape hatch
 * switches it back to measurement_active at compile time. */
void device_commands_measurement_begin(void)
{
    /* Acquire the PM lock on the outermost begin (0->1) so light sleep can't gate
     * the UART/I2C clocks while a burst is in flight; nested begins don't re-acquire. */
    if (s_measurement_active++ == 0 && s_no_ls_lock != NULL) {
        esp_pm_lock_acquire(s_no_ls_lock);
    }
}
void device_commands_measurement_end(void)
{
    /* Release only on a genuine 1->0 transition (balances the begin acquire). */
    if (s_measurement_active > 0 && --s_measurement_active == 0 && s_no_ls_lock != NULL) {
        esp_pm_lock_release(s_no_ls_lock);
    }
    if (s_measurement_active == 0) notify_sync();  /* burst done — let the runner drain */
}
bool device_commands_measurement_active(void) { return s_measurement_active > 0; }

static void device_commands_publish_hold_begin(void)
{
    s_publish_hold++;
}

static void device_commands_publish_hold_end(void)
{
    if (s_publish_hold > 0 && --s_publish_hold == 0) {
        /* A raw transaction can end while the outer Lua measurement bracket is
         * still active. Wake the runner here so the newly narrow gate matters. */
        notify_sync();
    }
}

bool device_commands_publish_hold_active(void) { return s_publish_hold > 0; }

static void sensor_transaction_begin(void)
{
    device_commands_measurement_begin();
    device_commands_publish_hold_begin();
}

static void sensor_transaction_end(void)
{
    device_commands_publish_hold_end();
    device_commands_measurement_end();
}

uint32_t device_commands_mqtt_error_disconnects(uint32_t window_s)
{
    return s_cfg.error_disconnect_count != NULL
        ? s_cfg.error_disconnect_count(window_s) : 0;
}

/* ── UART sensor commands — raw interface (Phase 7) ─────────────── */

/* Generic binary query: sends the 8-byte `cmd` (+ optional `extra` payload)
 * to an AMBIT sensor on `channel` (0-3) using the ambit-1 ESP protocol.
 *
 * `expect_raw` selects the response mode:
 *   UART_QUERY_ACK_ONLY  — return after CMD_DONE (0xA1); no response data, no CMD_END
 *   0                    — FSM data-transfer mode: wait for structured array data via
 *                          handshake (for measurement commands 20, 21)
 *   >0                   — immediate raw response: read exactly N bytes after CMD_DONE,
 *                          then verify CMD_END (0xF0) follows (for query commands 31-34)
 *
 * Caller must free response via uart_sensor_response_free().
 * Lua: device.uart_query(ch, {cmd_bytes}, extra_or_nil, expect_raw, timeout_ms)
 * CLI: not exposed directly (use typed ambit_* commands instead)                      */
cmd_result_t cmd_uart_query(uint8_t channel, const uint8_t cmd[8],
                            const uint8_t *extra, size_t extra_len,
                            size_t expect_raw,
                            uart_sensor_response_t *response,
                            uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (channel >= UART_SENSOR_NUM_CHANNELS) {
        return make_result(ESP_ERR_INVALID_ARG, "invalid channel %u", channel);
    }
    sensor_transaction_begin();
    esp_err_t err = s_cfg.uart_query(channel, cmd, extra, extra_len,
                                     expect_raw, response, timeout_ms);
    sensor_transaction_end();
    if (err != ESP_OK) {
        return make_result(err, "UART ch%u query failed: %s",
                           channel, esp_err_to_name(err));
    }
    if (expect_raw > 0) {
        return make_result(ESP_OK, "UART ch%u: %u raw bytes",
                           channel, (unsigned)response->raw_len);
    }
    return make_result(ESP_OK, "UART ch%u: %u arrays received",
                       channel, response->array_count);
}

/* Ping: send wake byte (0xAA) to the AMBIT sensor, wait for ack (0x80).
 * Result is cached for 10 seconds to avoid hammering the bus.
 * Lua:  device.uart_ping(ch) → true/false
 * CLI:  ping_uart <ch>                                                */
cmd_result_t cmd_uart_ping(uint8_t channel, bool *connected)
{
    if (!s_initialized || s_cfg.uart_ping == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (channel >= UART_SENSOR_NUM_CHANNELS) {
        return make_result(ESP_ERR_INVALID_ARG, "invalid channel %u", channel);
    }
    sensor_transaction_begin();
    esp_err_t err = s_cfg.uart_ping(channel, connected);
    sensor_transaction_end();
    if (err != ESP_OK) {
        return make_result(err, "UART ch%u ping failed: %s",
                           channel, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u: %s",
                       channel + 1, *connected ? "connected" : "disconnected");
}

/* Report connection state of all 4 UART channels.
 * Lua:  device.uart_status() → string
 * CLI:  uart_status                                                   */
cmd_result_t cmd_uart_status(void)
{
    if (!s_initialized || s_cfg.uart_status == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    static const char *state_str[] = { "disconnected", "connected", "busy" };
    char buf[200];
    int pos = 0;
    for (uint8_t ch = 0; ch < UART_SENSOR_NUM_CHANNELS; ch++) {
        uart_sensor_state_t st = UART_SENSOR_DISCONNECTED;
        s_cfg.uart_status(ch, &st);
        int n = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                         "AMBIT%u:%s ", ch + 1,
                         (st < 3) ? state_str[st] : "?");
        if (n > 0 && pos + n < (int)sizeof(buf)) {
            pos += n;
        }
    }
    return make_result(ESP_OK, "%s", buf);
}

/* Generic ASCII line-oriented query — sends "<cmd><terminator>", reads one
 * line back, and discards the first line if it echoes the command verbatim.
 * Transport/diagnostic only: NEVER stores (schema-v2 rule — measurement
 * commands store, transport commands don't). See device_commands.h. */
cmd_result_t cmd_uart_text_query(uint8_t channel,
                                 const char *cmd, const char *terminator,
                                 uint32_t timeout_ms,
                                 char *out_resp, size_t resp_cap,
                                 size_t *resp_len)
{
    if (!s_initialized || s_cfg.uart_text_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (channel >= UART_SENSOR_NUM_CHANNELS) {
        return make_result(ESP_ERR_INVALID_ARG, "invalid channel %u", channel);
    }
    if (cmd == NULL || terminator == NULL || out_resp == NULL ||
        resp_len == NULL || resp_cap < 2) {
        return make_result(ESP_ERR_INVALID_ARG, "bad args");
    }

    int64_t t0_us = esp_timer_get_time();  /* monotonic, for echo-retry budget */
    *resp_len = 0;
    out_resp[0] = '\0';

    /* First attempt — full timeout budget. */
    sensor_transaction_begin();
    esp_err_t err = s_cfg.uart_text_query(channel, cmd, terminator,
                                          out_resp, resp_cap, resp_len, timeout_ms);

    /* Echo handling: if the first line equals the sent cmd, throw it away and
     * read the next line with whatever budget is left. The line we just read
     * is already in out_resp without the terminator. */
    if (err == ESP_OK) {
        size_t cmd_len = strlen(cmd);
        if (*resp_len == cmd_len && memcmp(out_resp, cmd, cmd_len) == 0) {
            int64_t elapsed_us = esp_timer_get_time() - t0_us;
            int64_t remaining_ms = ((int64_t)timeout_ms * 1000 - elapsed_us) / 1000;
            if (remaining_ms <= 0) {
                err = ESP_ERR_TIMEOUT;
                *resp_len = 0;
                out_resp[0] = '\0';
            } else {
                /* Read another line — pass an empty cmd so nothing is re-sent. */
                *resp_len = 0;
                out_resp[0] = '\0';
                err = s_cfg.uart_text_query(channel, "", terminator,
                                            out_resp, resp_cap, resp_len,
                                            (uint32_t)remaining_ms);
            }
        }
    }
    sensor_transaction_end();

    /* On hard error (other than timeout) propagate. */
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return make_result(err, "uart_text_query ch%u failed: %s",
                           channel, esp_err_to_name(err));
    }

    if (err == ESP_ERR_TIMEOUT) {
        return make_result(ESP_ERR_TIMEOUT, "ch%u: no response within %ums",
                           channel, (unsigned)timeout_ms);
    }
    return make_result(ESP_OK, "ch%u: %u bytes", channel, (unsigned)*resp_len);
}

/* ── Typed Ambit commands ────────────────────────────────────────── *
 *
 * Each function wraps one ambit-1 ESP binary command (see ambit_protocol.h
 * for command IDs). The ambit-1 protocol is:
 *   Wake (0xAA×3) → Ack (0x80) → Header (0xA0) + 8-byte cmd [+extra]
 *   → CMD_DONE (0xA1) → [response] → [CMD_END (0xF0)]
 *
 * Four response patterns exist:
 *   ACK_ONLY  — cmds 1, 2, 10: CMD_DONE only, no CMD_END (config)
 *   RAW       — cmds 31-34:    CMD_DONE → N fixed bytes → CMD_END (query)
 *   FSM       — cmds 20, 21:   CMD_DONE → FSM handshake arrays → CMD_END (measurement)
 *   ACTION    — cmds 4-6, 17-18, 37: CMD_DONE → [work] → CMD_END (no data returned)
 * ────────────────────────────────────────────────────────────────── */

/* Helper: send an ack-only command (no response, no CMD_END) */
static cmd_result_t ambit_ack_only(uint8_t ch, const uint8_t cmd[8])
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     UART_QUERY_ACK_ONLY, &resp, 5000);
    uart_sensor_response_free(&resp);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u cmd %u failed: %s",
                           ch + 1, cmd[0], esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u cmd %u OK", ch + 1, cmd[0]);
}

/* Helper: send a command that returns CMD_END but no data (action) */
static cmd_result_t ambit_action(uint8_t ch, const uint8_t cmd[8],
                                 const uint8_t *extra, size_t extra_len,
                                 uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, extra, extra_len,
                                     0, &resp, timeout_ms);
    uart_sensor_response_free(&resp);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u cmd %u failed: %s",
                           ch + 1, cmd[0], esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u cmd %u OK", ch + 1, cmd[0]);
}

/* Per-channel AMBIT identity + config cache. Identity/calibration is fetched
 * once via cmd 33 (see ambit_info_fetch below); gains/currents are tracked here
 * at set-time. Written from the measurement (Lua) task, so no lock as long as
 * that stays the only writer. */
#define AMBIT_INFO_NUM_CH 4
static ambit_device_info_t s_ambit_info[AMBIT_INFO_NUM_CH];

/* Calibration-read retry state (2026-08-03 field defect): the AMBIT's human
 * name lives in the calibration blob, and the identity fetch used to treat a
 * failed calibration read as final — caching valid=true with an empty name, so
 * every measurement was labeled the generic "ambit" until the next reconnect or
 * reboot. After the v1.4.1 fleet OTA, 11 Ambits latched that way at once (the
 * post-reboot fetch races the AMBIT's own settling). Two-layer cure:
 *   1. retry the calibration read a few times inside the fetch itself;
 *   2. if it still fails, mark it pending here and keep re-trying on later
 *      lookups, rate-limited so a truly nameless/legacy AMBIT can't turn every
 *      measurement into a UART round-trip.
 * Same single-writer story as s_ambit_info (measurement/Lua task), so no lock. */
#define AMBIT_CAL_FETCH_TRIES     3      /* attempts inside one identity fetch */
#define AMBIT_CAL_RETRY_DELAY_MS  150    /* pause between those attempts */
#define AMBIT_CAL_RETRY_PERIOD_MS 60000  /* floor between later re-attempts */
static struct {
    bool    pending;          /* identity cached but calibration (name) unread */
    int64_t next_attempt_ms;  /* earliest time for the next re-attempt */
} s_ambit_cal_retry[AMBIT_INFO_NUM_CH];

/* Cmd 1 — Set photodetector gains on the ADPD6100.
 * Values 1-6 map to gain levels (0 = skip / keep current).
 * Must be called before cmd_ambit_config_detector() or cmd_ambit_run().
 * Lua:  device.ambit_set_gains(ch, fluo, fluoref, ir, irref, sun, leaf)
 * CLI:  (not exposed — use Lua)                                       */
cmd_result_t cmd_ambit_set_gains(uint8_t ch, uint8_t fluo, uint8_t fluoref,
                                  uint8_t ir, uint8_t irref,
                                  uint8_t sun, uint8_t leaf)
{
    uint8_t cmd[8] = { AMBIT_CMD_SET_GAINS, fluo, fluoref, ir, irref, sun, leaf, 0 };
    cmd_result_t r = ambit_ack_only(ch, cmd);
    /* Track at set-time — the AMBIT has no read-back, and we're the only setter,
     * so this stays authoritative for the event metadata. */
    if (r.status == ESP_OK && ch < AMBIT_INFO_NUM_CH) {
        s_ambit_info[ch].gains[0] = fluo;    s_ambit_info[ch].gains[1] = fluoref;
        s_ambit_info[ch].gains[2] = ir;      s_ambit_info[ch].gains[3] = irref;
        s_ambit_info[ch].gains[4] = sun;     s_ambit_info[ch].gains[5] = leaf;
        s_ambit_info[ch].gains_set = true;
    }
    return r;
}

/* Cmd 2 — Set LED drive currents (0-126).
 * i620 = 620nm pulsed, i720 = 720nm pulsed, ir = far-red DC.
 * Must be called before cmd_ambit_config_detector() or cmd_ambit_run().
 * Lua:  device.ambit_set_currents(ch, i620, i720, ir)                 */
cmd_result_t cmd_ambit_set_currents(uint8_t ch, uint8_t i620, uint8_t i720,
                                     uint8_t ir)
{
    uint8_t cmd[8] = { AMBIT_CMD_SET_CURRENTS, i620, i720, ir, 0, 0, 0, 0 };
    cmd_result_t r = ambit_ack_only(ch, cmd);
    if (r.status == ESP_OK && ch < AMBIT_INFO_NUM_CH) {
        s_ambit_info[ch].currents[0] = i620; s_ambit_info[ch].currents[1] = i720;
        s_ambit_info[ch].currents[2] = ir;
        s_ambit_info[ch].currents_set = true;
    }
    return r;
}

/* Cmd 10 — Apply stored gains and currents to the ADPD6100 detector.
 * Configures the detector into ARRAY_MODE1. Call after set_gains/set_currents,
 * or omit if cmd_ambit_run() auto-configures when mode differs.
 * Lua:  device.ambit_config_detector(ch)                              */
cmd_result_t cmd_ambit_config_detector(uint8_t ch)
{
    uint8_t cmd[8] = { AMBIT_CMD_CONFIG_DETECTOR, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ack_only(ch, cmd);
}

/* Cmd 32 — Read leaf and chip temperature from the MLX90632 IR sensor.
 * Returns temperatures in Celsius (ambit sends int16 × 10, we divide).
 * Response: 4 bytes (2 × int16_t little-endian).
 * Lua:  device.ambit_get_temp(ch) → {leaf=float, chip=float}
 * CLI:  ambit_temp <ch>                                               */
cmd_result_t cmd_ambit_get_temp(uint8_t ch, float *leaf_temp, float *chip_temp)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_GET_TEMP, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_TEMP_SIZE, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_TEMP_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_temp failed", ch + 1);
    }
    int16_t t1, t2;
    memcpy(&t1, resp.raw + 0, 2);
    memcpy(&t2, resp.raw + 2, 2);
    if (leaf_temp) *leaf_temp = (float)t1 / 10.0f;
    if (chip_temp) *chip_temp = (float)t2 / 10.0f;
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u T=%.1fC chip=%.1fC",
                       ch + 1, (float)t1 / 10.0f, (float)t2 / 10.0f);
}

/* Cmd 31 — Read spectral channels from the AS7341 and compute PAR.
 * Response: 24 bytes = 10 × uint16 channels + 1 × float32 PAR.
 * Lua:  device.ambit_get_spec(ch) → {spec={10 ints}, par=float}
 * CLI:  ambit_spec <ch>                                               */
cmd_result_t cmd_ambit_get_spec(uint8_t ch, uint16_t spec[10], float *par)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_GET_SPEC, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_SPEC_SIZE, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_SPEC_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_spec failed", ch + 1);
    }
    /* spec[0..9] are uint16 channels, spec[10..11] hold a float (PAR) */
    if (spec) memcpy(spec, resp.raw, 20);
    if (par)  memcpy(par, resp.raw + 20, 4);
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u spectrum OK", ch + 1);
}

/* Cmd 34 — Extended temperature read: two leaf algorithms + chip + 4 raw
 * MLX90632 register values for diagnostics.
 * Response: 14 bytes (7 × int16_t: leaf*10, leaf1*10, chip*10, a1..a4).
 * Lua:  device.ambit_get_temp_raw(ch) → {leaf,leaf1,chip,raw={4 ints}}
 * CLI:  (not exposed — use Lua or ambit_temp for basic reading)       */
cmd_result_t cmd_ambit_get_temp_raw(uint8_t ch, float *leaf, float *leaf1,
                                     float *chip, int16_t raw[4])
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_GET_TEMP_RAW, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_TEMP_RAW_SIZE, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_TEMP_RAW_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_temp_raw failed", ch + 1);
    }
    int16_t vals[7];
    memcpy(vals, resp.raw, 14);
    if (leaf)  *leaf  = (float)vals[0] / 10.0f;
    if (leaf1) *leaf1 = (float)vals[1] / 10.0f;
    if (chip)  *chip  = (float)vals[2] / 10.0f;
    if (raw) {
        raw[0] = vals[3]; raw[1] = vals[4];
        raw[2] = vals[5]; raw[3] = vals[6];
    }
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u T=%.1f/%.1f/%.1fC",
                       ch + 1, (float)vals[0]/10.f, (float)vals[1]/10.f, (float)vals[2]/10.f);
}

/* Cmd 33 — Retrieve sensor identity/calibration data.
 *   info_type=1: ambit_calibration_t (~136 B) — name, MLX coefficients,
 *                ADPD offsets, actinic/spec coefficients, emissivity
 *   info_type=2: ambit_fw_info_t (~48 B) — FW version, MAC, build date
 *   info_type=3: ambit_metadata_t (~248 B) — GPS, altitude, user notes
 * Raw struct bytes are copied into `out`. Struct layouts defined in
 * ambit_protocol.h (must match ambit-1 nvs1.h on ESP32 Xtensa alignment).
 * Lua:  device.ambit_get_info(ch, type) → raw bytes string
 * CLI:  ambit_info <ch> <1|2|3>                                       */
cmd_result_t cmd_ambit_get_info(uint8_t ch, uint8_t info_type,
                                 uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    size_t expect = 0;
    switch (info_type) {
    case AMBIT_INFO_CALIBRATION: expect = sizeof(ambit_calibration_t); break;
    case AMBIT_INFO_FW:          expect = sizeof(ambit_fw_info_t);     break;
    case AMBIT_INFO_METADATA:    expect = sizeof(ambit_metadata_t);    break;
    default:
        return make_result(ESP_ERR_INVALID_ARG, "info_type must be 1-3");
    }

    uint8_t cmd[8] = { AMBIT_CMD_GET_INFO, info_type, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0, expect, &resp, 5000);
    if (err != ESP_OK || resp.raw == NULL) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u get_info(%u) failed", ch + 1, info_type);
    }
    size_t copy = resp.raw_len < out_size ? resp.raw_len : out_size;
    if (out && copy > 0) memcpy(out, resp.raw, copy);
    if (out_len) *out_len = resp.raw_len;
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u info(%u): %u bytes",
                       ch + 1, info_type, (unsigned)resp.raw_len);
}

/* ── Cached AMBIT device identity (deviceID / fw_version / cal_version) ──────
 * The cache + setters live above (near cmd_ambit_set_gains); below is the
 * one-time fetch that fills identity/calibration and announces it. */

/* Emit one DEVICE_INFO event (tag DEVICE_INFO, channel uart_<ch>, device =
 * ambit_name, cmd_raw "get_info") carrying the full calibration. Called once
 * per connection from ambit_info_fetch. Best-effort: a store failure must not
 * fail the identity fetch. */
static void ambit_emit_device_info(uint8_t ch, const ambit_device_info_t *e,
                                   const ambit_calibration_t *cal, bool have_cal)
{
    if (s_cfg.store_event == NULL || s_cfg.next_id == NULL) return;

    char payload[768];
    int o = snprintf(payload, sizeof payload,
        "{\"device_id\":\"%s\",\"fw\":\"%s\",\"cal_version\":\"%08lx\"",
        e->device_id, e->fw_version, (unsigned long)e->cal_version);
    if (have_cal && o > 0 && o < (int)sizeof payload) {
        o += snprintf(payload + o, sizeof payload - o, ",\"mlx_coef\":[");
        for (int i = 0; i < 14 && o > 0 && o < (int)sizeof payload; i++)
            o += snprintf(payload + o, sizeof payload - o, "%s%ld",
                          i ? "," : "", (long)cal->mlx_coef[i]);
        o += snprintf(payload + o, sizeof payload - o, "],\"adpd\":[");
        for (int i = 0; i < 6 && o > 0 && o < (int)sizeof payload; i++)
            o += snprintf(payload + o, sizeof payload - o, "%s%lu",
                          i ? "," : "", (unsigned long)cal->adpd[i]);
        o += snprintf(payload + o, sizeof payload - o,
            "],\"temp_offset\":%.4f,\"temp_slope\":%.4f,\"actinic_coef\":%.6f,"
            "\"spec_coef\":%.6f,\"act\":[%u,%u,%u,%u,%u],"
            "\"mlx_emissivity\":%.4f,\"sun_coef\":%.6f,\"tick_factor\":%.6f",
            (double)cal->temp_offset, (double)cal->temp_slope, (double)cal->actinic_coef,
            (double)cal->spec_coef, (unsigned)cal->act_50, (unsigned)cal->act_100,
            (unsigned)cal->act_150, (unsigned)cal->act_200, (unsigned)cal->act_250,
            (double)cal->mlx_emissivity, (double)cal->sun_coef, (double)cal->tick_factor);
    }
    if (o < 0 || o >= (int)sizeof payload) {
        ESP_LOGW(TAG, "AMBIT%u device_info payload truncated", ch + 1);
        return;
    }

    int64_t mid = 0;
    if (s_cfg.next_id(&mid) != ESP_OK) return;
    char chan[12];
    snprintf(chan, sizeof chan, "uart_%u", (unsigned)ch);
    measurement_event_desc_t d = {
        .measure_id   = mid,
        .channel      = chan,
        .device       = (e->ambit_name[0] != '\0') ? e->ambit_name : "ambit",
        .tag          = MEASUREMENT_TAG_DEVICE_INFO,
        .cmd_raw      = "get_info",
        .start_ms     = now_ms(),
        .end_ms       = now_ms(),
        .payload_json = payload,
    };
    if (s_cfg.store_event(&d) == ESP_OK) {
        notify_sync();
        ESP_LOGI(TAG, "AMBIT%u DEVICE_INFO stored (%s cal=%08lx)",
                 ch + 1, e->ambit_name, (unsigned long)e->cal_version);
    }
}

/* One calibration-read attempt: on success fill the cal-derived fields of *e
 * (cal_version, actinic_coef, ambit_name) and return true, leaving *cal for the
 * DEVICE_INFO emit. On failure *e is untouched. */
static bool ambit_cal_read(uint8_t ch, ambit_device_info_t *e, ambit_calibration_t *cal)
{
    size_t cgot = 0;
    cmd_result_t cr = cmd_ambit_get_info(ch, AMBIT_INFO_CALIBRATION,
                                         (uint8_t *)cal, sizeof *cal, &cgot);
    if (cr.status != ESP_OK || cgot < sizeof *cal) return false;
    e->cal_version  = esp_rom_crc32_le(0, (const uint8_t *)cal, sizeof *cal);
    e->actinic_coef = cal->actinic_coef;
    memset(e->ambit_name, 0, sizeof e->ambit_name);
    memcpy(e->ambit_name, cal->ambit_name, sizeof e->ambit_name - 1);
    return true;
}

static esp_err_t ambit_info_fetch(uint8_t ch)
{
    /* FW info (MAC + version) is mandatory; calibration (→ cal_version) best-effort. */
    ambit_fw_info_t fw;
    size_t got = 0;
    cmd_result_t r = cmd_ambit_get_info(ch, AMBIT_INFO_FW, (uint8_t *)&fw, sizeof fw, &got);
    if (r.status != ESP_OK || got < sizeof fw) {
        return (r.status != ESP_OK) ? r.status : ESP_FAIL;
    }

    ambit_device_info_t e;
    memset(&e, 0, sizeof e);
    /* getEfuseMac() packs the 6-byte MAC little-endian (byte0 = low 8 bits). */
    uint64_t m = fw.mac;
    snprintf(e.device_id, sizeof e.device_id, "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)(m & 0xFF), (unsigned)((m >> 8) & 0xFF), (unsigned)((m >> 16) & 0xFF),
             (unsigned)((m >> 24) & 0xFF), (unsigned)((m >> 32) & 0xFF), (unsigned)((m >> 40) & 0xFF));
    snprintf(e.fw_version, sizeof e.fw_version, "%u.%u.%u",
             (unsigned)fw.major, (unsigned)fw.minor, (unsigned)fw.batch);
    e.hw_rev = fw.hw_rev;   /* 0 on pre-0.1.0 images (they never wrote the byte) */

    /* cal_version = CRC32 of the calibration struct → changes whenever the sensor
     * is recalibrated. The struct has no native version field.
     * Retried: the name lives in this blob, and a single boot-time timeout here
     * used to stick the channel with the generic "ambit" label for hours (see
     * s_ambit_cal_retry above). */
    ambit_calibration_t cal;
    bool have_cal = false;
    for (int attempt = 0; attempt < AMBIT_CAL_FETCH_TRIES && !have_cal; attempt++) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(AMBIT_CAL_RETRY_DELAY_MS));
        have_cal = ambit_cal_read(ch, &e, &cal);
    }
    /* Never latch a missing calibration as final: keep the identity usable
     * (fw/MAC below) but leave the name pending so later lookups re-try. */
    s_ambit_cal_retry[ch].pending         = !have_cal;
    s_ambit_cal_retry[ch].next_attempt_ms = now_ms() + AMBIT_CAL_RETRY_PERIOD_MS;

    /* Preserve gains/currents tracked since the last (re)connect — the identity
     * fetch must not clobber them (they live in the same cache struct). */
    e.gains_set     = s_ambit_info[ch].gains_set;
    memcpy(e.gains, s_ambit_info[ch].gains, sizeof e.gains);
    e.currents_set  = s_ambit_info[ch].currents_set;
    memcpy(e.currents, s_ambit_info[ch].currents, sizeof e.currents);

    e.valid = true;
    s_ambit_info[ch] = e;

    /* Announce the freshly-connected sensor's identity + calibration once. */
    ambit_emit_device_info(ch, &e, &cal, have_cal);
    return ESP_OK;
}

cmd_result_t cmd_ambit_device_info(uint8_t ch, ambit_device_info_t *out)
{
    if (ch >= AMBIT_INFO_NUM_CH || out == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "device_info: bad channel/arg");
    }
    if (!s_ambit_info[ch].valid) {
        esp_err_t err = ambit_info_fetch(ch);
        if (err != ESP_OK) {
            memset(out, 0, sizeof *out);
            return make_result(err, "AMBIT%u device_info fetch failed", ch + 1);
        }
    } else if (s_ambit_cal_retry[ch].pending &&
               now_ms() >= s_ambit_cal_retry[ch].next_attempt_ms) {
        /* Identity is cached but the calibration (and with it the name) never
         * arrived — measurements are going out labeled "ambit". Re-try on the
         * lookups that normal measurement traffic already makes, at most once
         * per AMBIT_CAL_RETRY_PERIOD_MS, and re-announce identity once the name
         * finally lands so the platform can correct itself. */
        s_ambit_cal_retry[ch].next_attempt_ms = now_ms() + AMBIT_CAL_RETRY_PERIOD_MS;
        ambit_calibration_t cal;
        if (ambit_cal_read(ch, &s_ambit_info[ch], &cal)) {
            s_ambit_cal_retry[ch].pending = false;
            ambit_emit_device_info(ch, &s_ambit_info[ch], &cal, true);
            ESP_LOGW(TAG, "AMBIT%u name recovered late: %s", ch + 1,
                     s_ambit_info[ch].ambit_name);
        }
    }
    *out = s_ambit_info[ch];
    return make_result(ESP_OK, "AMBIT%u %s fw=%s cal=%08lx",
                       ch + 1, out->device_id, out->fw_version, (unsigned long)out->cal_version);
}

bool cmd_ambit_device_info_cached(uint8_t ch, ambit_device_info_t *out)
{
    if (ch >= AMBIT_INFO_NUM_CH || out == NULL || !s_ambit_info[ch].valid) {
        return false;
    }
    *out = s_ambit_info[ch];
    return true;
}

void cmd_ambit_device_info_invalidate(uint8_t ch)
{
    if (ch < AMBIT_INFO_NUM_CH) {
        /* Drop identity (re-fetch + re-announce next use) AND tracked gains/
         * currents — the reconnected AMBIT booted with its own defaults, unknown
         * to us until the script sets them again. */
        s_ambit_info[ch].valid        = false;
        s_ambit_info[ch].gains_set    = false;
        s_ambit_info[ch].currents_set = false;
        /* The next fetch owns its own calibration retries. */
        s_ambit_cal_retry[ch].pending         = false;
        s_ambit_cal_retry[ch].next_attempt_ms = 0;
    }
}

/* Cmd 21 — Run an array-mode measurement on the ADPD6100.
 * `run_arr` is a flat byte array of arr_len × 8 bytes. Each 8-byte line
 * encodes: line_type, ir_on, sample_num(H), sample_num(L), freq(H),
 * freq(L), actinic, subsampling. Max 16 lines.
 * The extra payload is sent before CMD_DONE (buffered in ambit RX FIFO).
 * Response: FSM handshake — up to 7 data arrays (env, fluor, fluoref,
 * sun, leaf, 730, 730ref), each as {index, uint32_t[], length}.
 * Typical duration: seconds to ~60s depending on sample count.
 * Lua:  device.ambit_run(ch, flat_table, led_persist, allow_int, timeout)
 * CLI:  (not exposed — use Lua scripts for measurement workflows)     */
cmd_result_t cmd_ambit_run(uint8_t ch, const uint8_t *run_arr, uint8_t arr_len,
                            uint8_t led_persist, bool allow_interrupt,
                            uart_sensor_response_t *response, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (arr_len == 0 || arr_len > 16 || run_arr == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "arr_len must be 1-16");
    }
    uint8_t cmd[8] = { AMBIT_CMD_RUN, arr_len, led_persist,
                       (uint8_t)(allow_interrupt ? 1 : 0), 0, 0, 0, 0 };
    memset(response, 0, sizeof(*response));
    esp_err_t err = s_cfg.uart_query(ch, cmd, run_arr, (size_t)arr_len * 8,
                                     0, response, timeout_ms);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u run failed: %s",
                           ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u run: %u arrays",
                       ch + 1, response->array_count);
}

/* Cmd 20 — Run a multi-phase fluorescence (MPF) measurement.
 * `length` = total measurement points (uint16, encoded as [1]<<7 | [2]),
 * `interval` = sampling interval, `change_act`/`act` = actinic control.
 * Response: FSM handshake data arrays (same as cmd 21).
 * Lua:  device.ambit_run_mpf(ch, length, interval, change_act, act, timeout)
 * CLI:  (not exposed — use Lua)                                       */
cmd_result_t cmd_ambit_run_mpf(uint8_t ch, uint16_t length, uint8_t interval,
                                bool change_act, uint8_t act,
                                uart_sensor_response_t *response, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_RUN_MPF,
                       (uint8_t)(length >> 7), (uint8_t)(length & 0x7F),
                       interval, (uint8_t)(change_act ? 1 : 0), act, 0, 0 };
    memset(response, 0, sizeof(*response));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0, 0, response, timeout_ms);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u run_mpf failed: %s",
                           ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u mpf: %u arrays",
                       ch + 1, response->array_count);
}

/* ── Parallel measurement protocol (trigger → poll → fetch) ─────────────────
 * Lets the host start a run on every AMBIT back-to-back and collect them
 * afterwards, instead of blocking the whole run per channel. The four C3s
 * measure concurrently; the host only ever holds the (single shared) bus for a
 * short trigger/poll/fetch transaction. These deliberately do NOT bracket the
 * measurement gate — the Lua orchestrator asserts it once across the cycle. */

/* Cmd 22 — Trigger an async (retained) run. Same payload as cmd 21; the ambit
 * acks CMD_DONE (ACK_ONLY) and then runs into its own buffers, staying silent
 * until FETCH. Keep timeout_ms short — this only covers wake + ack. */
cmd_result_t cmd_ambit_trigger(uint8_t ch, const uint8_t *run_arr, uint8_t arr_len,
                               uint8_t led_persist, bool allow_interrupt,
                               uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (arr_len == 0 || arr_len > 16 || run_arr == NULL) {
        return make_result(ESP_ERR_INVALID_ARG, "arr_len must be 1-16");
    }
    uint8_t cmd[8] = { AMBIT_CMD_RUN_START, arr_len, led_persist,
                       (uint8_t)(allow_interrupt ? 1 : 0), 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, run_arr, (size_t)arr_len * 8,
                                     UART_QUERY_ACK_ONLY, &resp, timeout_ms);
    uart_sensor_response_free(&resp);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u trigger failed: %s", ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u triggered", ch + 1);
}

/* Cmd 23 — Poll async run state into *state (AMBIT_ASYNC_IDLE|DONE|ERROR). A
 * measuring ambit doesn't answer, so ESP_ERR_TIMEOUT here means "busy" — the
 * caller maps it. Keep timeout_ms short (a few wake retries) so a busy/locked
 * channel fails fast instead of spraying wake bytes at a measuring sensor. */
cmd_result_t cmd_ambit_poll(uint8_t ch, uint8_t *state, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    if (state) *state = 0xFF;   /* unknown until the ambit answers */
    uint8_t cmd[8] = { AMBIT_CMD_STATUS, 0, 0, 0, 0, 0, 0, 0 };
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0,
                                     AMBIT_RESP_STATUS_SIZE, &resp, timeout_ms);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < AMBIT_RESP_STATUS_SIZE) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u poll: no answer", ch + 1);
    }
    if (state) *state = resp.raw[0];
    uint8_t st = resp.raw[0];
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u state=%u", ch + 1, (unsigned)st);
}

/* Cmd 24 — Fetch the retained run result. The ambit streams its buffered arrays
 * back over the FSM exactly like cmd 21, so `response` is identical to
 * cmd_ambit_run's output. timeout_ms must cover the stream (scale with size). */
cmd_result_t cmd_ambit_fetch(uint8_t ch, uart_sensor_response_t *response,
                             uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uint8_t cmd[8] = { AMBIT_CMD_FETCH, 0, 0, 0, 0, 0, 0, 0 };
    memset(response, 0, sizeof(*response));
    esp_err_t err = s_cfg.uart_query(ch, cmd, NULL, 0, 0, response, timeout_ms);
    if (err != ESP_OK) {
        return make_result(err, "AMBIT%u fetch failed: %s", ch + 1, esp_err_to_name(err));
    }
    return make_result(ESP_OK, "AMBIT%u fetch: %u arrays", ch + 1, response->array_count);
}

/* Cmd 5 — Blink the AS7341 LED for visual identification.
 * `ambit_id` 0-3 selects blink pattern, `intensity` 5-253 sets brightness.
 * Blocks until blink completes (a few seconds).
 * Lua:  device.ambit_blink(ch, id, intensity)
 * CLI:  ambit_blink <ch> <id> <intensity>                             */
cmd_result_t cmd_ambit_blink(uint8_t ch, uint8_t ambit_id, uint8_t intensity)
{
    uint8_t cmd[8] = { AMBIT_CMD_BLINK, ambit_id, intensity, 0, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, NULL, 0, 10000);
}

/* Cmd 6 — Run ADPD6100 fluorescence offset calibration.
 * Measures dark baseline and stores offsets (adpd_lit, adpd_sun, adpd_leaf,
 * adpd_730, adpd_730r) in the ambit's NVS. Factory/maintenance command.
 * Blocks for several seconds.
 * Lua:  device.ambit_calibrate_baseline(ch)                           */
cmd_result_t cmd_ambit_calibrate_baseline(uint8_t ch)
{
    uint8_t cmd[8] = { AMBIT_CMD_CALIBRATE_BASELINE, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, NULL, 0, 30000);
}

/* Cmd 4 — Actinic LED control and calibration.
 *   type=1: test actinics — ramps LED current (var), blocks ~6s
 *   type=2: set actinic coefficient (float packed in cmd[3..6])
 *   type=4: set spectral coefficient
 *   type=5: pulse LED at `var` current for `var2`×100 ms
 * Factory/calibration command.
 * Lua:  device.ambit_actinic(ch, type, var, var2)                     */
cmd_result_t cmd_ambit_actinic(uint8_t ch, uint8_t type, uint8_t var, uint8_t var2)
{
    uint8_t cmd[8] = { AMBIT_CMD_ACTINIC, type, var, var2, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, NULL, 0, 15000);
}

/* Cmd 37 — Write metadata (GPS coordinates, altitude, user notes) to
 * the ambit's NVS. `metadata` should be a serialised ambit_metadata_t
 * (~248 bytes). The struct's eof_mark must be 2025 for the ambit to
 * accept the write. Data is sent as `extra` before CMD_DONE; the ambit
 * reads it from its UART RX FIFO after acknowledging.
 * Lua:  device.ambit_set_metadata(ch, metadata_string)                */
cmd_result_t cmd_ambit_set_metadata(uint8_t ch, const uint8_t *metadata, size_t len)
{
    uint8_t cmd[8] = { AMBIT_CMD_SET_METADATA, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_action(ch, cmd, metadata, len, 5000);
}

/* ── AMBIT OTA-over-UART (cmds 25-28) ───────────────────────────────────────
 * Each command sends its 8-byte header (+ optional chunk payload) and reads the
 * AMBIT's 1-byte status reply (DONE + status + END). The ambit_ota component
 * orchestrates begin → data* → end; these only frame one exchange. */

static uint16_t ambit_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Send one OTA command (cmd[8] + optional extra) and read the 1-byte status. */
static cmd_result_t ambit_ota_cmd(uint8_t ch, const uint8_t cmd[8],
                                  const uint8_t *extra, size_t extra_len,
                                  uint8_t *status, uint32_t timeout_ms)
{
    if (!s_initialized || s_cfg.uart_query == NULL) {
        return make_result(ESP_ERR_NOT_SUPPORTED, "UART sensors not available");
    }
    uart_sensor_response_t resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = s_cfg.uart_query(ch, cmd, extra, extra_len,
                                     1 /* 1-byte status */, &resp, timeout_ms);
    if (err != ESP_OK || resp.raw == NULL || resp.raw_len < 1) {
        uart_sensor_response_free(&resp);
        return make_result(err != ESP_OK ? err : ESP_FAIL,
                           "AMBIT%u OTA cmd %u: no answer", ch + 1, cmd[0]);
    }
    uint8_t st = resp.raw[0];
    if (status) *status = st;
    uart_sensor_response_free(&resp);
    return make_result(ESP_OK, "AMBIT%u OTA cmd %u status=%u", ch + 1, cmd[0], st);
}

cmd_result_t cmd_ambit_ota_begin(uint8_t ch, uint32_t image_size, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_BEGIN,
                       (uint8_t)image_size,         (uint8_t)(image_size >> 8),
                       (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
                       0, 0, 0 };
    /* begin() erases the target OTA partition (~hundreds of ms) — allow time. */
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 15000);
}

cmd_result_t cmd_ambit_ota_data(uint8_t ch, uint16_t seq,
                                const uint8_t *chunk, uint8_t len, uint8_t *status)
{
    if (chunk == NULL || len == 0 || len > AMBIT_OTA_CHUNK_MAX) {
        return make_result(ESP_ERR_INVALID_ARG, "OTA chunk len must be 1-%d", AMBIT_OTA_CHUNK_MAX);
    }
    uint8_t cmd[8] = { AMBIT_CMD_OTA_DATA, len,
                       (uint8_t)seq, (uint8_t)(seq >> 8), 0, 0, 0, 0 };
    uint8_t extra[AMBIT_OTA_CHUNK_MAX + 2];
    memcpy(extra, chunk, len);
    uint16_t crc = ambit_crc16_ccitt(chunk, len);
    extra[len]     = (uint8_t)crc;
    extra[len + 1] = (uint8_t)(crc >> 8);
    return ambit_ota_cmd(ch, cmd, extra, (size_t)len + 2, status, 5000);
}

cmd_result_t cmd_ambit_ota_end(uint8_t ch, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_END, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 10000);
}

cmd_result_t cmd_ambit_ota_abort(uint8_t ch, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_ABORT, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 5000);
}

cmd_result_t cmd_ambit_ota_confirm(uint8_t ch, uint8_t *status)
{
    uint8_t cmd[8] = { AMBIT_CMD_OTA_CONFIRM, 0, 0, 0, 0, 0, 0, 0 };
    return ambit_ota_cmd(ch, cmd, NULL, 0, status, 5000);
}

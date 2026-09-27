/* ESP-IDF glue for sd_diag (see sd_diag.h). The block lives in RTC_NOINIT so
 * the counts survive every CPU reset the SD faults themselves can provoke
 * (panics, watchdogs, our own fault-injection resets); NVS on internal flash
 * holds a rate-limited floor for power-on. No function here touches the SD. */
#include "sd_diag.h"

#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG      "sd_diag"
#define NVS_NS   "sd_diag"
#define KEY_SNAP "snap"

static RTC_NOINIT_ATTR sd_diag_block_t s_blk;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* Serializes a whole snapshot transaction (decide, copy, NVS set+commit,
 * persisted-state update). The spinlock alone is not enough: nvs_set_blob IS
 * the flash write, so a heartbeat snapshot copied before a fault could land
 * AFTER the forced reboot snapshot of that fault and regress the floor (eval
 * round 3: saved gen 3 over gen 4, fsync count lost). Static and created in
 * boot_early, before any task exists, so taking it can never fail. */
static StaticSemaphore_t s_persist_buf;
static SemaphoreHandle_t s_persist_mtx;
static uint32_t s_persisted_gen;
static bool     s_persisted_this_boot;
static uint32_t s_last_persist_ms;
static bool     s_nvs_ready;

static uint32_t uptime_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void sd_diag_boot_early(void)
{
    /* Only a reset that preserves RTC memory can continue a block, and the
     * CRC is the proof it did; brown-out qualifies only when the CRC agrees. */
    esp_reset_reason_t r = esp_reset_reason();
    bool cpu = r != ESP_RST_POWERON && r != ESP_RST_UNKNOWN && r != ESP_RST_EXT;
    if (s_persist_mtx == NULL) s_persist_mtx = xSemaphoreCreateMutexStatic(&s_persist_buf);
    portENTER_CRITICAL(&s_mux);
    sd_diag_core_boot(&s_blk, cpu);
    portEXIT_CRITICAL(&s_mux);
}

void sd_diag_boot_nvs(void)
{
    /* Read-only: an unreadable, missing or malformed snapshot is simply "no
     * floor" and the merge stays inexact (fail closed). */
    sd_diag_block_t floor;
    bool have_floor = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof floor;
        have_floor = nvs_get_blob(h, KEY_SNAP, &floor, &len) == ESP_OK && len == sizeof floor && sd_diag_valid(&floor);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    sd_diag_core_merge_floor(&s_blk, have_floor ? &floor : NULL);
    /* A continued (CPU-reset) block already holds everything up to the last
     * snapshot; only changes after that snapshot are unpersisted. */
    s_persisted_gen = have_floor ? floor.gen : 0;
    s_persisted_this_boot = false;   /* per-boot policy state: a boot is a boot */
    s_last_persist_ms = 0;
    if (!have_floor && s_blk.gen == 0) s_persisted_gen = 0;
    s_nvs_ready = true;
    portEXIT_CRITICAL(&s_mux);
}

void sd_diag_fault(sd_diag_writer_t w, sd_diag_op_t op, int err)
{
    uint32_t t = uptime_ms();
    portENTER_CRITICAL_SAFE(&s_mux);
    sd_diag_core_fault(&s_blk, w, op, err, t);
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void sd_diag_refusal(sd_diag_refusal_t reason, int64_t id, int err, uint8_t blocked, uint8_t sd_state)
{
    uint32_t t = uptime_ms();
    int64_t wall = (int64_t)time(NULL) * 1000LL;
    portENTER_CRITICAL_SAFE(&s_mux);
    sd_diag_core_refusal(&s_blk, reason, id, err, blocked, sd_state, wall, t);
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void sd_diag_get(sd_diag_block_t *out)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    *out = s_blk;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void sd_diag_persist(bool force)
{
    if (!s_nvs_ready) return;
    /* Taken BEFORE the decision: a forced caller that arrives while a
     * heartbeat snapshot is in flight waits for it, then decides and copies
     * afresh, so the last write is always the newest block. */
    xSemaphoreTake(s_persist_mtx, portMAX_DELAY);
    sd_diag_block_t copy;
    uint32_t now = uptime_ms();
    portENTER_CRITICAL(&s_mux);
    bool go = sd_diag_core_should_persist(&s_blk, s_persisted_gen, s_persisted_this_boot, s_last_persist_ms, now, force);
    copy = s_blk;
    portEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (go && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e = nvs_set_blob(h, KEY_SNAP, &copy, sizeof copy);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
        if (e == ESP_OK) {
            portENTER_CRITICAL(&s_mux);
            s_persisted_gen = copy.gen;
            s_persisted_this_boot = true;
            s_last_persist_ms = now;
            portEXIT_CRITICAL(&s_mux);
        }
    }
    xSemaphoreGive(s_persist_mtx);
}

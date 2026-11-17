/* Bench-only read-only diagnostics over BLE (contract v0,
 * docs/bench/ble-diagnostics.md). Built only in env bench-ble.
 *
 * What it may touch, and why (CLAUDE.md task boundaries):
 *  - The snapshot is the heartbeat sample object device_commands ALREADY built
 *    (telemetry_built hook), copied here. Serving it never builds a status
 *    report: that runs a charger read, whose side effects (ADC start, history
 *    counters, re-detect/AICO after-read hooks) must not depend on a phone.
 *  - `live` is the CACHED last charger frame (mp2731_last_frame_at). That call
 *    takes the charger read mutex, which an I2C read holds for up to ~1 s, so
 *    only this component's own worker task calls it; the NimBLE host task
 *    reads the worker's copy under a spinlock.
 *  - NimBLE host callbacks take only s_mux (a few hundred bytes of memcpy at
 *    most), never event_log's mutex, never a FreeRTOS wait.
 *  - Nothing here writes NVS: no bonding store (BT_NIMBLE_NVS_PERSIST off,
 *    SM off), and the window is RAM state.
 */

#include "ble_diag.h"
#include "ble_diag_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "mp2731.h"
#include "mp2731_core.h"
#include "sensing_port.h"

#define WINDOW_DEFAULT_MIN 15u
#define WINDOW_MAX_MIN     1440u
#define WORKER_PERIOD_MS   1000u
#define WORKER_STACK       3584u
#define WORKER_PRIORITY    1u
#define PREFERRED_MTU      247u
/* 100-150 ms: discoverable within a second for a phone, at a modest radio
 * duty cycle beside Wi-Fi (software coexistence time-shares the one antenna). */
#define ADV_ITVL_MIN       BLE_GAP_ADV_ITVL_MS(100)
#define ADV_ITVL_MAX       BLE_GAP_ADV_ITVL_MS(150)

static const char *TAG = "ble_diag";

/* 6a4d00NN-3b2c-4f8e-9a51-0c7e2b4d9a10, NimBLE's little-endian byte order. */
#define DIAG_UUID(n) BLE_UUID128_INIT(0x10, 0x9a, 0x4d, 0x2b, 0x7e, 0x0c, 0x51, 0x9a, \
                                      0x8e, 0x4f, 0x2c, 0x3b, (n), 0x00, 0x4d, 0x6a)
static const ble_uuid128_t SVC_UUID    = DIAG_UUID(0x01);
static const ble_uuid128_t META_UUID   = DIAG_UUID(0x02);
static const ble_uuid128_t OFFSET_UUID = DIAG_UUID(0x03);
static const ble_uuid128_t PAGE_UUID   = DIAG_UUID(0x04);
static const ble_uuid128_t LIVE_UUID   = DIAG_UUID(0x05);

enum { CHR_META = 1, CHR_OFFSET, CHR_PAGE, CHR_LIVE };

/* s_mux guards the served snapshot fields (active/len/seq/built_ms), the live
 * cache, and the small radio-state fields below. Hold times are bounded by one
 * 240-byte page copy. */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_write_mtx;   /* serialises snapshot writers */
static ble_diag_snap_t   s_snap;
static bool              s_inited;
static uint32_t          s_dropped;     /* heartbeats not captured (busy/oversize) */

typedef struct {
    bool           have;
    mp2731_frame_t frame;
    esp_err_t      err;
    int64_t        captured_ms;
} live_cache_t;
static live_cache_t s_live;

static bool     s_started;
static bool     s_synced;
static bool     s_advertising;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu;
static int64_t  s_window_end_ms;        /* uptime; 0 = closed */
static uint32_t s_page_offset;          /* the one connection's page cursor */
static uint8_t  s_own_addr_type;
static uint32_t s_connects, s_meta_reads, s_page_reads, s_live_reads;

static uint16_t s_meta_handle, s_offset_handle, s_page_handle, s_live_handle;
static char     s_name[12];
static char     s_mac[18];
static char     s_fw[BLE_DIAG_FW_MAX + 1];

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ── Snapshot (writer side: the task that built the heartbeat) ────────── */

esp_err_t ble_diag_init(void)
{
    if (s_inited) return ESP_OK;
    s_write_mtx = xSemaphoreCreateMutex();
    if (s_write_mtx == NULL) return ESP_ERR_NO_MEM;
    for (int i = 0; i < 2; i++) {
        /* 2 x 8 KiB: PSRAM first, the internal heap is the scarce one. */
        s_snap.buf[i] = heap_caps_malloc(BLE_DIAG_SNAPSHOT_CAP,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_snap.buf[i] == NULL)
            s_snap.buf[i] = heap_caps_malloc(BLE_DIAG_SNAPSHOT_CAP, MALLOC_CAP_8BIT);
        if (s_snap.buf[i] == NULL) {
            free(s_snap.buf[0]);
            s_snap.buf[0] = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_snap.cap = BLE_DIAG_SNAPSHOT_CAP;
    s_inited = true;
    return ESP_OK;
}

void ble_diag_snapshot_set(const char *json, size_t len)
{
    if (!s_inited || json == NULL) return;
    if (xSemaphoreTake(s_write_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_dropped++;
        return;
    }
    /* Staging is the buffer no reader is served from; only writers (holding
     * s_write_mtx) ever change `active`, so reading it here is stable. */
    if (ble_diag_snap_stage(&s_snap, json, len)) {
        const int64_t now = uptime_ms();
        taskENTER_CRITICAL(&s_mux);
        ble_diag_snap_publish(&s_snap, len, now);
        taskEXIT_CRITICAL(&s_mux);
    } else {
        s_dropped++;
    }
    xSemaphoreGive(s_write_mtx);
}

/* ── GATT ─────────────────────────────────────────────────────────────── */

static int append(struct os_mbuf *om, const void *data, size_t len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int render_meta(char *out, size_t cap)
{
    taskENTER_CRITICAL(&s_mux);
    const uint32_t seq = s_snap.seq;
    const uint32_t len = s_snap.len;
    const int64_t built = s_snap.built_ms;
    taskEXIT_CRITICAL(&s_mux);
    const int64_t age = seq != 0u ? uptime_ms() - built : -1;
    return ble_diag_render_meta(out, cap, seq, len, age, s_fw, s_mac);
}

static int render_live(char *out, size_t cap)
{
    taskENTER_CRITICAL(&s_mux);
    const live_cache_t c = s_live;
    taskEXIT_CRITICAL(&s_mux);
    ble_diag_live_t l = {.have = c.have && c.captured_ms >= 0};
    if (l.have) {
        power_reading_t p;
        memset(&p, 0, sizeof p);
        mp2731_core_decode(&c.frame, &p);
        l.age_ms = uptime_ms() - c.captured_ms;
        l.vin_mv = p.input_mv;
        l.vsys_mv = p.system_mv;
        l.vbat_mv = p.battery_mv;
        l.iin_ma = p.input_ma;
        l.ichg_ma = p.charge_ma;
        l.status = c.frame.status;
        l.fault = c.frame.fault;
        l.valid = c.err == ESP_OK;
        l.reason = power_read_reason_name(power_read_reason_from_err(c.err));
    }
    return ble_diag_render_live(out, cap, &l);
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                     void *arg)
{
    (void)conn;
    (void)attr;
    switch ((uintptr_t)arg) {
    case CHR_META: {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
        char buf[BLE_DIAG_META_MAX + 1];
        const int n = render_meta(buf, sizeof buf);
        s_meta_reads++;
        return n < 0 ? BLE_ATT_ERR_UNLIKELY : append(ctxt->om, buf, (size_t)n);
    }
    case CHR_OFFSET: {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
        uint8_t raw[4];
        uint16_t got = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) != sizeof raw ||
            ble_hs_mbuf_to_flat(ctxt->om, raw, sizeof raw, &got) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint32_t offset = 0;
        if (!ble_diag_parse_offset(raw, got, &offset))
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        taskENTER_CRITICAL(&s_mux);
        s_page_offset = offset;
        taskEXIT_CRITICAL(&s_mux);
        return 0;
    }
    case CHR_PAGE: {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
        uint8_t page[BLE_DIAG_PAGE_MAX];
        taskENTER_CRITICAL(&s_mux);
        const size_t n = ble_diag_snap_page(&s_snap, s_page_offset, page);
        taskEXIT_CRITICAL(&s_mux);
        s_page_reads++;
        return append(ctxt->om, page, n);
    }
    case CHR_LIVE: {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
        char buf[BLE_DIAG_LIVE_MAX + 1];
        const int n = render_live(buf, sizeof buf);
        s_live_reads++;
        return n < 0 ? BLE_ATT_ERR_UNLIKELY : append(ctxt->om, buf, (size_t)n);
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &META_UUID.u, .access_cb = access_cb, .arg = (void *)CHR_META,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_meta_handle},
            {.uuid = &OFFSET_UUID.u, .access_cb = access_cb, .arg = (void *)CHR_OFFSET,
             .flags = BLE_GATT_CHR_F_WRITE, .val_handle = &s_offset_handle},
            {.uuid = &PAGE_UUID.u, .access_cb = access_cb, .arg = (void *)CHR_PAGE,
             .flags = BLE_GATT_CHR_F_READ, .val_handle = &s_page_handle},
            {.uuid = &LIVE_UUID.u, .access_cb = access_cb, .arg = (void *)CHR_LIVE,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_live_handle},
            {0},
        },
    },
    {0},
};

/* ── GAP ──────────────────────────────────────────────────────────────── */

static int gap_event(struct ble_gap_event *event, void *arg);

static int64_t window_remaining_ms(void)
{
    taskENTER_CRITICAL(&s_mux);
    const int64_t end = s_window_end_ms;
    taskEXIT_CRITICAL(&s_mux);
    const int64_t left = end - uptime_ms();
    return end != 0 && left > 0 ? left : 0;
}

/* Legacy advertising: flags + the 128-bit service UUID (21 of 31 bytes) in the
 * advertisement, the name in the scan response (it does not fit beside the
 * UUID). Runs on the worker or the host task; NimBLE's API is host-locked. */
static void advertise(void)
{
    const int64_t left = window_remaining_ms();
    if (!s_synced || left <= 0 || s_conn != BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_adv_active()) {
        return;
    }
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof params);
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = ADV_ITVL_MIN;
    params.itvl_max = ADV_ITVL_MAX;
    /* The controller ends advertising at the window edge by itself; the
     * worker re-checks every second in case a start raced the edge. */
    const int32_t duration = left > INT32_MAX ? INT32_MAX : (int32_t)left;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, duration, &params, gap_event, NULL);
    if (rc == 0) {
        s_advertising = true;
        ESP_LOGI(TAG, "advertising as %s for %" PRId32 " s", s_name, duration / 1000);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv start rc=%d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_advertising = false;
        if (event->connect.status == 0) {
            taskENTER_CRITICAL(&s_mux);
            s_conn = event->connect.conn_handle;
            s_page_offset = 0;
            s_mtu = 23;
            taskEXIT_CRITICAL(&s_mux);
            s_connects++;
            ESP_LOGI(TAG, "connected (handle %u)", (unsigned)event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        taskENTER_CRITICAL(&s_mux);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_mtu = 0;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        advertise();   /* only while the window is open */
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        ESP_LOGI(TAG, "advertising ended (reason %d)", event->adv_complete.reason);
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "mtu %u", (unsigned)event->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d", (unsigned)event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no BLE identity address rc=%d", rc);
        return;
    }
    s_synced = true;
    advertise();
}

static void on_reset(int reason)
{
    s_synced = false;
    s_advertising = false;
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();               /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ── Worker: window edge, cached charger frame, change notifications ──── */

static void worker_task(void *arg)
{
    (void)arg;
    uint32_t notified_seq = 0;
    int64_t notified_frame_ms = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WORKER_PERIOD_MS));
        if (s_synced) {
            if (window_remaining_ms() <= 0) {
                if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
                s_advertising = false;
            } else {
                advertise();
            }
        }

        /* Cache copy only: no I2C, no ADC start, no history or re-detect
         * hook. May wait on the charger read mutex; this task can afford it. */
        mp2731_frame_t frame;
        int64_t captured = -1;
        const esp_err_t err = mp2731_last_frame_at(&frame, &captured);
        if (captured >= 0 && captured != notified_frame_ms) {
            notified_frame_ms = captured;
            taskENTER_CRITICAL(&s_mux);
            s_live = (live_cache_t){.have = true, .frame = frame, .err = err,
                                    .captured_ms = captured};
            taskEXIT_CRITICAL(&s_mux);
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gatts_chr_updated(s_live_handle);
        }

        taskENTER_CRITICAL(&s_mux);
        const uint32_t seq = s_snap.seq;
        taskEXIT_CRITICAL(&s_mux);
        if (seq != notified_seq) {
            notified_seq = seq;
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gatts_chr_updated(s_meta_handle);
        }
    }
}

/* ── Console: ble | ble on [min] | ble off ────────────────────────────── */

static void set_window_min(uint32_t minutes)
{
    const int64_t end = minutes ? uptime_ms() + (int64_t)minutes * 60000 : 0;
    taskENTER_CRITICAL(&s_mux);
    s_window_end_ms = end;
    taskEXIT_CRITICAL(&s_mux);
}

static void print_status(void)
{
    taskENTER_CRITICAL(&s_mux);
    const uint32_t seq = s_snap.seq, len = s_snap.len;
    const int64_t built = s_snap.built_ms;
    const live_cache_t live = s_live;
    const uint16_t conn = s_conn, mtu = s_mtu;
    taskEXIT_CRITICAL(&s_mux);
    const int64_t now = uptime_ms();
    printf("ble: %s synced=%d adv=%d window_left=%llds conn=%s mtu=%u connects=%" PRIu32 "\n",
           s_name, s_synced, ble_gap_adv_active() ? 1 : 0,
           (long long)(window_remaining_ms() / 1000),
           conn == BLE_HS_CONN_HANDLE_NONE ? "none" : "up", (unsigned)mtu, s_connects);
    printf("ble: snapshot seq=%" PRIu32 " len=%" PRIu32 " age=%llds dropped=%" PRIu32
           " | live age=%llds reason=%s\n",
           seq, len, seq ? (long long)((now - built) / 1000) : -1LL, s_dropped,
           live.have ? (long long)((now - live.captured_ms) / 1000) : -1LL,
           live.have ? power_read_reason_name(power_read_reason_from_err(live.err)) : "none");
    printf("ble: reads meta=%" PRIu32 " page=%" PRIu32 " live=%" PRIu32 "\n",
           s_meta_reads, s_page_reads, s_live_reads);
    printf("ble: heap internal free=%u largest=%u min=%u | psram free=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static int ble_cmd(int argc, char **argv)
{
    if (argc <= 1) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        uint32_t minutes = WINDOW_DEFAULT_MIN;
        if (argc > 2) {
            char *end = NULL;
            const unsigned long v = strtoul(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || v < 1 || v > WINDOW_MAX_MIN) {
                printf("ble on [1..%u minutes]\n", WINDOW_MAX_MIN);
                return 1;
            }
            minutes = (uint32_t)v;
        }
        set_window_min(minutes);
        /* A running advertisement carries the old duration: restart it. */
        if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
        s_advertising = false;
        advertise();
        printf("ble: window open for %" PRIu32 " min\n", minutes);
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        set_window_min(0);
        if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
        s_advertising = false;
        const uint16_t conn = s_conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE)
            (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        printf("ble: off (advertising stopped%s)\n",
               conn != BLE_HS_CONN_HANDLE_NONE ? ", link closed" : "");
        return 0;
    }
    printf("usage: ble | ble on [minutes] | ble off\n");
    return 1;
}

static const esp_console_cmd_t s_ble_cmd = {
    .command = "ble",
    .help = "ble | ble on [min] | ble off  bench BLE diagnostics (read-only)",
    .hint = NULL,
    .func = ble_cmd,
};

esp_err_t ble_diag_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;
    ble_diag_format_name(mac, s_name);
    ble_diag_format_mac(mac, s_mac);
    snprintf(s_fw, sizeof s_fw, "%s", esp_app_get_description()->version);

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_name);
    if (rc == 0) rc = ble_att_set_preferred_mtu(PREFERRED_MTU);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup rc=%d", rc);
        return ESP_FAIL;
    }
    set_window_min(WINDOW_DEFAULT_MIN);

    err = esp_console_cmd_register(&s_ble_cmd);
    if (err != ESP_OK) ESP_LOGW(TAG, "`ble` command: %s", esp_err_to_name(err));
    if (xTaskCreate(worker_task, "ble_diag", WORKER_STACK, NULL, WORKER_PRIORITY, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGW(TAG, "BENCH BLE diagnostics on: %s, window %u min (not a release feature)",
             s_name, WINDOW_DEFAULT_MIN);
    return ESP_OK;
}

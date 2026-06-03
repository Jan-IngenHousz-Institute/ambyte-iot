/*
 * usb_sensors.c — USB-OTG host driver for CDC-ACM line sensors (CO2Dot).
 *
 * Up to USB_SENSOR_NUM_CHANNELS identical CDC-ACM devices hang off ONE powered
 * hub. They share VID/PID/serial, so a logical channel is bound to a fixed
 * PHYSICAL hub downstream port (s_port_map) read from usb_device_info_t.parent.
 *
 * Structure:
 *   - usb_host_install() + a daemon task pumping usb_host_lib_handle_events().
 *   - cdc_acm_host_install() spawns the CDC client task internally.
 *   - new-dev callback (USB-host context — must NOT open there) reads the port
 *     + device address and posts a connect request to s_connect_q.
 *   - a connector task drains the queue and opens the device by address, binding
 *     the resulting handle to the slot for its physical port.
 *   - per-device RX data_cb assembles a line and signals line_ready; the device
 *     event_cb handles disconnect.
 *
 * text_query() is the only data path: write "<cmd><terminator>", wait for one
 * terminator-terminated line. The CO2Dot answers exactly one JSON line per
 * command, so a single line_ready signal per query is correct.
 */

#include "usb_sensors.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#define TAG "usb_sens"

/* The CO2Dot is an ESP32-C3, which has NO USB-OTG — its Arduino USB-CDC runs
 * over the chip's built-in USB-Serial-JTAG peripheral, so it enumerates with
 * Espressif's fixed id 303a:1001 'USB JTAG/serial debug unit', NOT the
 * 16c0:0483 from the [env:usb] build flags (those only apply to TinyUSB on
 * S2/S3). So we do NOT filter by VID/PID — we open by device address only
 * (CDC_HOST_ANY_VID/PID), since each device is already pinned by dev_addr +
 * physical hub port. */
#define CO2DOT_INTERFACE_IDX  0

#define USB_RX_LINE_CAP       512      /* one JSON reply line */
#define USB_TX_BUF_SIZE       128
#define USB_IN_BUF_SIZE       512
#define USB_OPEN_TIMEOUT_MS   2000

#define USB_DAEMON_TASK_STACK     4096
#define USB_DAEMON_TASK_PRIO      2
#define USB_CONNECTOR_TASK_STACK  4096
#define USB_CONNECTOR_TASK_PRIO   3     /* > app tasks per cdc_acm_host docs */
#define USB_CDC_DRIVER_TASK_STACK 4096
#define USB_CDC_DRIVER_TASK_PRIO  5

/* Logical channel ⇄ physical hub downstream port (1-based).
 * VERIFY on hardware: plug one unit per port and confirm parent.port_num. */
static const uint8_t s_port_map[USB_SENSOR_NUM_CHANNELS] = {1, 2, 3, 4};

typedef struct {
    bool                connected;
    uint8_t             port_num;       /* physical hub port (1-based) */
    uint8_t             dev_addr;        /* current USB address (per enumeration) */
    cdc_acm_dev_hdl_t   cdc_hdl;
    char                rx_line[USB_RX_LINE_CAP];
    size_t              rx_len;
    bool                rx_overflow;
    SemaphoreHandle_t   line_ready;      /* given by data_cb on terminator */
    SemaphoreHandle_t   io_lock;         /* serialize tx+rx per slot */
} usb_slot_t;

static usb_slot_t s_slots[USB_SENSOR_NUM_CHANNELS];
static SemaphoreHandle_t s_slots_mtx;        /* guards slot connect/disconnect + s_seen */
static QueueHandle_t     s_connect_q;        /* new-device connect requests */
static bool              s_inited;

/* Connect request posted from the (host-context) new-dev callback. */
typedef struct {
    uint8_t dev_addr;
    uint8_t port_num;
} connect_req_t;

/* Diagnostic cache: descriptor info captured for every device the new-dev
 * callback sees (its handle is briefly valid there), keyed by USB address.
 * usb_sensors_scan() cross-references this against the live device list.
 * Guarded by s_slots_mtx. */
#define USB_SCAN_MAX     8
#define USB_PRODUCT_CAP  32
typedef struct {
    bool     used;
    uint8_t  dev_addr;
    uint8_t  port_num;
    uint16_t vid;
    uint16_t pid;
    char     product[USB_PRODUCT_CAP];
} usb_seen_t;
static usb_seen_t s_seen[USB_SCAN_MAX];

/* ── helpers ─────────────────────────────────────────────────────────── */

static int channel_for_port(uint8_t port_num)
{
    for (int i = 0; i < USB_SENSOR_NUM_CHANNELS; i++) {
        if (s_port_map[i] == port_num) {
            return i;
        }
    }
    return -1;
}

/* Copy a UTF-16LE USB string descriptor to a printable ASCII buffer. */
static void str_desc_ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (d == NULL || d->bLength < 2) {
        return;
    }
    int chars = (d->bLength - 2) / 2;
    size_t n = 0;
    for (int i = 0; i < chars && n < cap - 1; i++) {
        uint16_t c = d->wData[i];
        out[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[n] = '\0';
}

/* Record/refresh a device's descriptor info in the scan cache (by address). */
static void seen_record(uint8_t addr, uint8_t port, uint16_t vid, uint16_t pid,
                        const char *product)
{
    xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
    usb_seen_t *e = NULL;
    for (int i = 0; i < USB_SCAN_MAX; i++) {           /* reuse same-addr entry */
        if (s_seen[i].used && s_seen[i].dev_addr == addr) { e = &s_seen[i]; break; }
    }
    if (e == NULL) {
        for (int i = 0; i < USB_SCAN_MAX; i++) {       /* else first free slot */
            if (!s_seen[i].used) { e = &s_seen[i]; break; }
        }
    }
    if (e != NULL) {
        e->used = true;
        e->dev_addr = addr;
        e->port_num = port;
        e->vid = vid;
        e->pid = pid;
        snprintf(e->product, sizeof(e->product), "%s", product ? product : "");
    }
    xSemaphoreGive(s_slots_mtx);
}

/* Caller must hold s_slots_mtx. */
static const usb_seen_t *seen_find(uint8_t addr)
{
    for (int i = 0; i < USB_SCAN_MAX; i++) {
        if (s_seen[i].used && s_seen[i].dev_addr == addr) {
            return &s_seen[i];
        }
    }
    return NULL;
}

/* ── RX + device-event callbacks (per opened device) ─────────────────── */

static bool on_rx(const uint8_t *data, size_t data_len, void *user_arg)
{
    usb_slot_t *slot = (usb_slot_t *)user_arg;

    for (size_t i = 0; i < data_len; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            /* Terminate the assembled line and wake the waiting query. */
            slot->rx_line[slot->rx_len] = '\0';
            xSemaphoreGive(slot->line_ready);
            slot->rx_len = 0;
            slot->rx_overflow = false;
        } else if (c == '\r') {
            /* ignore CR */
        } else if (slot->rx_len < USB_RX_LINE_CAP - 1) {
            slot->rx_line[slot->rx_len++] = c;
        } else {
            slot->rx_overflow = true;   /* drop until next '\n' */
        }
    }
    return true;   /* data consumed; flush the driver RX buffer */
}

static void on_cdc_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    usb_slot_t *slot = (usb_slot_t *)user_ctx;

    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "ch%d (port %u) disconnected",
                 channel_for_port(slot->port_num), slot->port_num);
        xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
        cdc_acm_dev_hdl_t hdl = slot->cdc_hdl;
        slot->connected = false;
        slot->cdc_hdl   = NULL;
        slot->dev_addr  = 0;
        xSemaphoreGive(s_slots_mtx);
        /* Unblock any in-flight text_query waiting on this slot. */
        xSemaphoreGive(slot->line_ready);
        if (hdl) {
            cdc_acm_host_close(hdl);
        }
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGW(TAG, "ch%d CDC error %d",
                 channel_for_port(slot->port_num), event->data.error);
    }
}

/* ── connector task: opens devices posted by the new-dev callback ────── */

static void usb_connector_task(void *arg)
{
    (void)arg;
    connect_req_t req;
    while (xQueueReceive(s_connect_q, &req, portMAX_DELAY) == pdTRUE) {
        int ch = channel_for_port(req.port_num);
        if (ch < 0) {
            ESP_LOGW(TAG, "device on unmapped hub port %u (addr %u) — ignored",
                     req.port_num, req.dev_addr);
            continue;
        }
        usb_slot_t *slot = &s_slots[ch];

        /* Close a stale handle if this slot somehow re-enumerated. */
        xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
        cdc_acm_dev_hdl_t stale = slot->connected ? slot->cdc_hdl : NULL;
        if (stale) {
            slot->connected = false;
            slot->cdc_hdl   = NULL;
        }
        xSemaphoreGive(s_slots_mtx);
        if (stale) {
            cdc_acm_host_close(stale);
        }

        cdc_acm_host_open_config_t open_cfg = {
            .vid                 = CDC_HOST_ANY_VID,   /* C3 enumerates as 303a:1001 */
            .pid                 = CDC_HOST_ANY_PID,
            .interface_idx       = CO2DOT_INTERFACE_IDX,
            .dev_addr            = req.dev_addr,   /* select THIS device by address */
            .connection_timeout_ms = USB_OPEN_TIMEOUT_MS,
            .out_buffer_size     = USB_TX_BUF_SIZE,
            .in_buffer_size      = USB_IN_BUF_SIZE,
            .event_cb            = on_cdc_event,
            .data_cb             = on_rx,
            .user_arg            = slot,
        };
        cdc_acm_dev_hdl_t hdl = NULL;
        esp_err_t err = cdc_acm_host_open(&open_cfg, &hdl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open ch%d (port %u addr %u) failed: %s",
                     ch, req.port_num, req.dev_addr, esp_err_to_name(err));
            continue;
        }

        xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
        slot->port_num    = req.port_num;
        slot->dev_addr    = req.dev_addr;
        slot->cdc_hdl     = hdl;
        slot->rx_len      = 0;
        slot->rx_overflow = false;
        slot->connected   = true;
        xSemaphoreGive(s_slots_mtx);
        /* Drain any stale line_ready from a previous life. */
        xSemaphoreTake(slot->line_ready, 0);
        ESP_LOGI(TAG, "ch%d bound: port %u, addr %u", ch, req.port_num, req.dev_addr);
    }
    vTaskDelete(NULL);
}

/* ── new-device callback (USB-host context: do NOT open here) ────────── */

static void on_new_usb_dev(usb_device_handle_t usb_dev)
{
    usb_device_info_t info;
    if (usb_host_device_info(usb_dev, &info) != ESP_OK) {
        ESP_LOGW(TAG, "new dev: usb_host_device_info failed");
        return;
    }

    /* Capture descriptor info for usb_scan while the handle is valid. */
    uint16_t vid = 0, pid = 0;
    const usb_device_desc_t *desc = NULL;
    if (usb_host_get_device_descriptor(usb_dev, &desc) == ESP_OK && desc != NULL) {
        vid = desc->idVendor;
        pid = desc->idProduct;
    }
    char product[USB_PRODUCT_CAP];
    str_desc_ascii(info.str_desc_product, product, sizeof(product));
    seen_record(info.dev_addr, info.parent.port_num, vid, pid, product);

    /* port_num == 0 means attached directly to the root port (no hub). */
    connect_req_t req = { .dev_addr = info.dev_addr, .port_num = info.parent.port_num };
    ESP_LOGI(TAG, "new USB dev: addr %u, hub port %u, %04x:%04x '%s'",
             req.dev_addr, req.port_num, vid, pid, product);
    if (xQueueSend(s_connect_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "connect queue full — dropped addr %u", req.dev_addr);
    }
}

/* ── host-library daemon task ────────────────────────────────────────── */

static void usb_host_daemon_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        /* We never uninstall at runtime, so NO_CLIENTS / ALL_FREE are not acted
         * on here; the loop simply keeps servicing the host library. */
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

esp_err_t usb_sensors_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    s_slots_mtx = xSemaphoreCreateMutex();
    s_connect_q = xQueueCreate(USB_SENSOR_NUM_CHANNELS * 2, sizeof(connect_req_t));
    if (s_slots_mtx == NULL || s_connect_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < USB_SENSOR_NUM_CHANNELS; i++) {
        s_slots[i].line_ready = xSemaphoreCreateBinary();
        s_slots[i].io_lock    = xSemaphoreCreateMutex();
        s_slots[i].port_num   = s_port_map[i];
        if (s_slots[i].line_ready == NULL || s_slots[i].io_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const usb_host_config_t host_cfg = {
        .skip_phy_setup     = false,   /* use internal PHY (forces VBUS valid) */
        .root_port_unpowered = false,  /* power the root port on install */
        .intr_flags         = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t host_err = usb_host_install(&host_cfg);
    if (host_err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(host_err));
        return host_err;
    }

    if (xTaskCreate(usb_host_daemon_task, "usb_daemon", USB_DAEMON_TASK_STACK,
                    NULL, USB_DAEMON_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const cdc_acm_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = USB_CDC_DRIVER_TASK_STACK,
        .driver_task_priority   = USB_CDC_DRIVER_TASK_PRIO,
        .xCoreID                = 0,
        .new_dev_cb             = on_new_usb_dev,
    };
    esp_err_t err = cdc_acm_host_install(&drv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(usb_connector_task, "usb_connect", USB_CONNECTOR_TASK_STACK,
                    NULL, USB_CONNECTOR_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "USB host ready (%d CDC-ACM channels, by hub port)",
             USB_SENSOR_NUM_CHANNELS);
    return ESP_OK;
}

static esp_err_t usb_text_query(uint8_t channel, const char *cmd,
                                const char *terminator,
                                char *out_resp, size_t resp_cap,
                                size_t *resp_len, uint32_t timeout_ms)
{
    if (channel >= USB_SENSOR_NUM_CHANNELS || cmd == NULL ||
        out_resp == NULL || resp_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (terminator == NULL) {
        terminator = "\n";
    }
    if (resp_len) {
        *resp_len = 0;
    }
    out_resp[0] = '\0';

    usb_slot_t *slot = &s_slots[channel];
    if (xSemaphoreTake(slot->io_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    if (!slot->connected || slot->cdc_hdl == NULL) {
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }

    /* Reset RX assembly and drain any stale line_ready. */
    slot->rx_len = 0;
    slot->rx_overflow = false;
    xSemaphoreTake(slot->line_ready, 0);

    /* Build "<cmd><terminator>" and send. */
    {
        char txbuf[USB_TX_BUF_SIZE];
        int n = snprintf(txbuf, sizeof(txbuf), "%s%s", cmd, terminator);
        if (n < 0 || (size_t)n >= sizeof(txbuf)) {
            ret = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        ret = cdc_acm_host_data_tx_blocking(slot->cdc_hdl, (const uint8_t *)txbuf,
                                            (size_t)n, timeout_ms);
        if (ret != ESP_OK) {
            goto done;
        }
    }

    /* Wait for one assembled line. */
    if (xSemaphoreTake(slot->line_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        goto done;
    }
    if (!slot->connected) {     /* disconnected while waiting */
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }

    {
        size_t copy = strlen(slot->rx_line);
        if (copy > resp_cap - 1) {
            copy = resp_cap - 1;
        }
        memcpy(out_resp, slot->rx_line, copy);
        out_resp[copy] = '\0';
        if (resp_len) {
            *resp_len = copy;
        }
    }
    ret = ESP_OK;

done:
    xSemaphoreGive(slot->io_lock);
    return ret;
}

static esp_err_t usb_ping(uint8_t channel, bool *connected)
{
    if (channel >= USB_SENSOR_NUM_CHANNELS || connected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *connected = s_slots[channel].connected;
    return ESP_OK;
}

static esp_err_t usb_status(uint8_t channel, usb_sensor_state_t *out)
{
    if (channel >= USB_SENSOR_NUM_CHANNELS || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_slots[channel].connected ? USB_SENSOR_CONNECTED : USB_SENSOR_DISCONNECTED;
    return ESP_OK;
}

esp_err_t usb_sensors_scan(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_inited) {
        snprintf(out, cap, "USB host not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t addrs[USB_SCAN_MAX];
    int n = 0;
    esp_err_t err = usb_host_device_addr_list_fill(USB_SCAN_MAX, addrs, &n);
    if (err != ESP_OK) {
        snprintf(out, cap, "addr_list_fill failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t pos = 0;
    pos += snprintf(out + pos, cap - pos, "USB devices on bus: %d", n);
    if (n == 0) {
        snprintf(out + pos, cap - pos,
                 "\n  (nothing enumerated — check hub power, OTG cable, VBUS)");
        return ESP_OK;
    }

    xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
    for (int i = 0; i < n && pos < cap - 1; i++) {
        const usb_seen_t *s = seen_find(addrs[i]);
        int w;
        if (s != NULL) {
            int ch = channel_for_port(s->port_num);
            if (ch >= 0) {
                w = snprintf(out + pos, cap - pos,
                             "\n  addr %2u  hub-port %u  %04x:%04x '%s'  -> ch%d",
                             addrs[i], s->port_num, s->vid, s->pid, s->product, ch);
            } else {
                w = snprintf(out + pos, cap - pos,
                             "\n  addr %2u  hub-port %u  %04x:%04x '%s'  -> UNMAPPED (add %u to s_port_map)",
                             addrs[i], s->port_num, s->vid, s->pid, s->product, s->port_num);
            }
        } else {
            w = snprintf(out + pos, cap - pos,
                         "\n  addr %2u  (no cached descriptor — likely the hub itself)",
                         addrs[i]);
        }
        if (w <= 0) {
            break;
        }
        pos += ((size_t)w < cap - pos) ? (size_t)w : (cap - pos - 1);
    }
    xSemaphoreGive(s_slots_mtx);
    return ESP_OK;
}

usb_sensor_text_query_fn usb_sensors_get_text_query_fn(void) { return usb_text_query; }
usb_sensor_ping_fn       usb_sensors_get_ping_fn(void)       { return usb_ping; }
usb_sensor_status_fn     usb_sensors_get_status_fn(void)     { return usb_status; }

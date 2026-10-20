#include "svc_usb.h"
#include "svc_wire.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"

/* The pinned IDF driver creates RX before TX during install. Capture once at
 * that public boundary rather than reaching into its private object layout.
 * Traffic hooks for every other ring do just the comparison and tail-call. */
static RingbufHandle_t s_usb_rx;
static TaskHandle_t s_install_task;
static bool s_active;
static bool s_overrun;
static uint32_t s_dropped_calls, s_dropped_bytes;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static void *s_borrowed;
static size_t s_borrowed_length;
static linenoise_read_bytes_fn s_editor_read;
static svc_editor_filter_t s_filter;
static unsigned char s_pending[9];
static size_t s_pending_at, s_pending_length;
static void (*s_notice)(const char *);

esp_err_t __real_usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config);
RingbufHandle_t __real_xRingbufferCreate(size_t size, RingbufferType_t type);
BaseType_t __real_xRingbufferSendFromISR(RingbufHandle_t ring, const void *data,
                                       size_t length, BaseType_t *woken);
void *__real_xRingbufferReceiveUpTo(RingbufHandle_t ring, size_t *length,
                                   TickType_t wait, size_t maximum);
void __real_vRingbufferReturnItem(RingbufHandle_t ring, void *item);
int __real_usb_serial_jtag_write_bytes(const void *data, size_t length, TickType_t wait);

esp_err_t __wrap_usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    s_install_task = xTaskGetCurrentTaskHandle();
    esp_err_t error = __real_usb_serial_jtag_driver_install(config);
    s_install_task = NULL;
    return error;
}

RingbufHandle_t __wrap_xRingbufferCreate(size_t size, RingbufferType_t type)
{
    RingbufHandle_t ring = __real_xRingbufferCreate(size, type);
    if (!s_usb_rx && s_install_task && s_install_task == xTaskGetCurrentTaskHandle())
        s_usb_rx = ring;
    return ring;
}

BaseType_t IRAM_ATTR __wrap_xRingbufferSendFromISR(RingbufHandle_t ring,
                                                 const void *data, size_t length,
                                                 BaseType_t *woken)
{
    if (ring != s_usb_rx) return __real_xRingbufferSendFromISR(ring, data, length, woken);
    BaseType_t result = __real_xRingbufferSendFromISR(ring, data, length, woken);
    if (result == pdFALSE) {
        portENTER_CRITICAL_ISR(&s_mux);
        s_overrun = true;
        portEXIT_CRITICAL_ISR(&s_mux);
    }
    return result;
}

void *__wrap_xRingbufferReceiveUpTo(RingbufHandle_t ring, size_t *length,
                                   TickType_t wait, size_t maximum)
{
    if (ring != s_usb_rx) return __real_xRingbufferReceiveUpTo(ring, length, wait, maximum);
    void *item = __real_xRingbufferReceiveUpTo(ring, length, wait, maximum);
    s_borrowed = item;
    s_borrowed_length = item ? *length : 0;
    return item;
}

void __wrap_vRingbufferReturnItem(RingbufHandle_t ring, void *item)
{
    if (ring != s_usb_rx) {
        __real_vRingbufferReturnItem(ring, item);
        return;
    }
    if (item == s_borrowed) {
        svc_wipe(item, s_borrowed_length);
        s_borrowed = NULL;
        s_borrowed_length = 0;
    }
    __real_vRingbufferReturnItem(ring, item);
}

int __wrap_usb_serial_jtag_write_bytes(const void *data, size_t length, TickType_t wait)
{
    if (!svc_usb_active()) return __real_usb_serial_jtag_write_bytes(data, length, wait);
    portENTER_CRITICAL(&s_mux);
    if (s_dropped_calls != UINT32_MAX) ++s_dropped_calls;
    if (length > UINT32_MAX - s_dropped_bytes) s_dropped_bytes = UINT32_MAX;
    else s_dropped_bytes += (uint32_t)length;
    portEXIT_CRITICAL(&s_mux);
    return (int)length;
}

static ssize_t editor_read(int fd, void *buffer, size_t capacity)
{
    if (!capacity) return 0;
    for (;;) {
        if (s_pending_at < s_pending_length) {
            size_t n = s_pending_length - s_pending_at;
            if (n > capacity) n = capacity;
            memcpy(buffer, s_pending + s_pending_at, n);
            svc_wipe(s_pending + s_pending_at, n);
            s_pending_at += n;
            return (ssize_t)n;
        }
        unsigned char byte = 0;
        ssize_t n = s_editor_read(fd, &byte, 1);
        if (n <= 0) return n;
        bool dropped;
        s_pending_length = svc_editor_byte(&s_filter, byte, s_pending, &dropped);
        s_pending_at = 0;
        byte = 0;
        if (dropped && s_notice) s_notice("mode_required");
        /* A zero read ends a linenoise edit. Keep waiting rather than exposing
         * a partial withheld prefix as EOF, a prompt, or command history. */
    }
}

/* The linked linenoise object calls its own setter internally, so --wrap on
 * the setter cannot intercept that reference. This is the library's public
 * weak customization hook, and keeps the pinned image's blocking-read setup. */
void linenoiseSetReadCharacteristics(void)
{
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    s_editor_read = read;
    linenoiseSetReadFunction(editor_read);
}

void svc_usb_notice_handler(void (*handler)(const char *reason)) { s_notice = handler; }

bool svc_usb_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active = s_active;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

bool svc_usb_enter(void)
{
    fflush(stdout);
    fflush(stderr);
    portENTER_CRITICAL(&s_mux);
    s_active = true;
    s_overrun = false;
    portEXIT_CRITICAL(&s_mux);
    return usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000)) == ESP_OK;
}

void svc_usb_leave(void)
{
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
    portENTER_CRITICAL(&s_mux);
    s_active = false;
    portEXIT_CRITICAL(&s_mux);
}

bool svc_usb_take_overrun(void)
{
    portENTER_CRITICAL(&s_mux);
    bool result = s_overrun;
    s_overrun = false;
    portEXIT_CRITICAL(&s_mux);
    return result;
}

void svc_usb_counters(uint32_t *calls, uint32_t *bytes)
{
    portENTER_CRITICAL(&s_mux);
    *calls = s_dropped_calls;
    *bytes = s_dropped_bytes;
    portEXIT_CRITICAL(&s_mux);
}

int svc_usb_read(void *buffer, size_t length, unsigned timeout_ms)
{
    return usb_serial_jtag_read_bytes(buffer, (uint32_t)length, pdMS_TO_TICKS(timeout_ms));
}

bool svc_usb_write(const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    int64_t deadline = esp_timer_get_time() + 1000000;
    while (length) {
        size_t n = length > 64 ? 64 : length;
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return false;
        int wrote = __real_usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS((remaining + 999) / 1000));
        if (wrote <= 0) return false;
        p += wrote;
        length -= (size_t)wrote;
    }
    int64_t remaining = deadline - esp_timer_get_time();
    return remaining > 0 && usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS((remaining + 999) / 1000)) == ESP_OK;
}

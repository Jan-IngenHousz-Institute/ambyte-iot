/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../components/usb_service/svc_usb.c"

struct svc_test_ring { unsigned char bytes[256]; size_t length; bool returned; };
static struct svc_test_ring rings[6];
static unsigned ring_count, critical, notices;
static char output[8192];
static size_t output_length;
static linenoise_read_bytes_fn editor;
static int64_t now;

void svc_test_enter(void) { assert(critical++ == 0); }
void svc_test_exit(void) { assert(critical-- == 1); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
int64_t esp_timer_get_time(void) { return ++now * 1000; }

RingbufHandle_t __real_xRingbufferCreate(size_t size, RingbufferType_t type)
{
    (void)type; assert(size == 256 && ring_count < 6);
    return &rings[ring_count++];
}
esp_err_t __real_usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    __wrap_xRingbufferCreate((size_t)config->rx_buffer_size, RINGBUF_TYPE_BYTEBUF);
    __wrap_xRingbufferCreate((size_t)config->tx_buffer_size, RINGBUF_TYPE_BYTEBUF);
    return ESP_OK;
}
BaseType_t __real_xRingbufferSendFromISR(RingbufHandle_t ring, const void *data,
                                       size_t length, BaseType_t *woken)
{
    (void)woken; assert(!critical);
    if (length > sizeof ring->bytes - ring->length) return pdFALSE;
    memcpy(ring->bytes + ring->length, data, length); ring->length += length;
    return pdTRUE;
}
void *__real_xRingbufferReceiveUpTo(RingbufHandle_t ring, size_t *length,
                                   TickType_t wait, size_t maximum)
{
    (void)wait; assert(!critical);
    *length = ring->length < maximum ? ring->length : maximum;
    return *length ? ring->bytes : NULL;
}
void __real_vRingbufferReturnItem(RingbufHandle_t ring, void *item)
{
    assert(item == ring->bytes && !critical);
    if (ring == s_usb_rx) for (size_t i = 0; i < s_borrowed_length; ++i) assert(ring->bytes[i] == 0);
    ring->length = 0; ring->returned = true;
}
int __real_usb_serial_jtag_write_bytes(const void *bytes, size_t length, TickType_t wait)
{
    (void)wait; assert(!critical && length <= sizeof output - output_length);
    memcpy(output + output_length, bytes, length); output_length += length;
    return (int)length;
}
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t wait) { (void)wait; return ESP_OK; }
int usb_serial_jtag_read_bytes(void *buffer, uint32_t maximum, TickType_t wait)
{
    /* Same public driver boundary sequence: receive, copy, return. Target
     * disassembly independently verifies these calls reach these wrappers. */
    size_t n = 0;
    void *data = __wrap_xRingbufferReceiveUpTo(s_usb_rx, &n, wait, maximum);
    if (!data) return 0;
    memcpy(buffer, data, n);
    __wrap_vRingbufferReturnItem(s_usb_rx, data);
    return (int)n;
}
void linenoiseSetReadFunction(linenoise_read_bytes_fn function) { editor = function; }
static void notice(const char *reason) { assert(!strcmp(reason, "mode_required")); ++notices; }

int main(void)
{
    usb_serial_jtag_driver_config_t config = {256,256};
    assert(__wrap_usb_serial_jtag_driver_install(&config) == ESP_OK);
    assert(s_usb_rx == &rings[0]);
    BaseType_t woken = 0;
    for (unsigned channel = 0; channel < 4; ++channel) {
        RingbufHandle_t ring = __wrap_xRingbufferCreate(256, RINGBUF_TYPE_BYTEBUF);
        unsigned char uart[144]; memset(uart, (int)channel + 1, sizeof uart);
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            assert(__wrap_xRingbufferSendFromISR(ring, uart, sizeof uart, &woken) == pdTRUE);
            size_t n; void *bytes = __wrap_xRingbufferReceiveUpTo(ring, &n, 0, 144);
            assert(n == 144 && memcmp(bytes, uart, n) == 0);
            __wrap_vRingbufferReturnItem(ring, bytes);
            assert(memcmp(ring->bytes, uart, sizeof uart) == 0);
        }
    }
    const char secret[] = "BASE64-and-raw-password-sentinel";
    assert(__wrap_xRingbufferSendFromISR(s_usb_rx, secret, sizeof secret, &woken));
    char readback[sizeof secret];
    assert(svc_usb_read(readback, sizeof readback, 0) == sizeof secret);
    assert(memcmp(readback, secret, sizeof secret) == 0);
    for (size_t i = 0; i < sizeof secret; ++i) assert(s_usb_rx->bytes[i] == 0);
    unsigned char full[256] = {0};
    assert(__wrap_xRingbufferSendFromISR(s_usb_rx, full, sizeof full, &woken));
    assert(!__wrap_xRingbufferSendFromISR(s_usb_rx, full, 1, &woken));
    assert(svc_usb_take_overrun() && !svc_usb_take_overrun());
    assert(svc_usb_enter());
    size_t before = output_length;
    assert(__wrap_usb_serial_jtag_write_bytes(secret, sizeof secret, 0) == sizeof secret);
    assert(output_length == before);
    assert(svc_usb_write("framed", 6));
    assert(output_length == before + 6);
    uint32_t calls, bytes; svc_usb_counters(&calls, &bytes);
    assert(calls == 1 && bytes == sizeof secret);
    svc_usb_leave();
    assert(__wrap_usb_serial_jtag_write_bytes("legacy", 6, 0) == 6);
    svc_usb_notice_handler(notice);
    int descriptors[2]; assert(pipe(descriptors) == 0);
    const char *input = "00000001 svc.wifi.join password-sentinel\nidentity\n";
    assert(write(descriptors[1], input, strlen(input)) == (ssize_t)strlen(input));
    close(descriptors[1]); assert(dup2(descriptors[0], STDIN_FILENO) >= 0); close(descriptors[0]);
    linenoiseSetReadCharacteristics();
    char received[32] = {0}; size_t length = 0;
    while (editor(STDIN_FILENO, received + length, 1) > 0) ++length;
    assert(length == strlen("identity\n") && !memcmp(received, "identity\n", length));
    assert(notices == 1 && critical == 0);
    for (size_t i = 0; i < sizeof s_pending; ++i) assert(s_pending[i] == 0);
    puts("USB adapter boundaries and four UART rings PASS");
}

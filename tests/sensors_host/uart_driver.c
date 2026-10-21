/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "uart_sensors.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

const char *esp_err_to_name(esp_err_t error) { (void)error; return "test_error"; }
static _Atomic int64_t clock_us;
static pthread_mutex_t inventory_mutex = PTHREAD_MUTEX_INITIALIZER;
static int mutexes[5], mutex_count;
static unsigned char rx[512], payload[140];
static size_t rx_at, rx_size;
static unsigned writes, refresh_starts, mode;
static bool absent, command_pending, race;
static _Atomic bool waiting;
static pthread_t acquisition;
static void lock(void) { assert(!pthread_mutex_lock(&inventory_mutex)); }
static void unlock(void) { assert(!pthread_mutex_unlock(&inventory_mutex)); }
int64_t esp_timer_get_time(void) { return clock_us; }
static int64_t utc(void) { return 1792531200000LL; }
static uint8_t quality(bool *suspect) { *suspect = false; return 3; }
void vTaskDelay(TickType_t ms) { clock_us += (int64_t)ms * 1000; waiting = true; sched_yield(); }
static void yield(uint32_t ms) { vTaskDelay(ms); }
void identity_host_enter(portMUX_TYPE *mux) { (void)mux; }
void identity_host_exit(portMUX_TYPE *mux) { (void)mux; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { assert(mutex_count < 5); return &mutexes[mutex_count++]; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t p, TickType_t wait)
{ int *held = p; if (*held) { clock_us += (int64_t)wait * 1000; return 0; } *held = 1; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t p) { int *held = p; assert(*held); *held = 0; return pdTRUE; }
esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_set_level(int pin, unsigned value) { (void)pin; (void)value; return ESP_OK; }
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config) { assert(port == 0 && config); return ESP_OK; }
esp_err_t uart_driver_install(uart_port_t port, int r, int t, int q, void *h, int flags)
{ (void)r; (void)t; (void)q; (void)h; (void)flags; assert(port == 0); return ESP_OK; }
esp_err_t uart_set_pin(uart_port_t port, int tx, int r, int a, int b)
{ (void)tx; (void)r; (void)a; (void)b; assert(port == 0); return ESP_OK; }
esp_err_t uart_flush_input(uart_port_t port)
{ assert(port == 0); rx_size = rx_at = 0; return ESP_OK; }
static void *run(void *unused)
{ (void)unused; assert(sensor_inventory_acquire(3)); return NULL; }
int uart_write_bytes(uart_port_t port, const void *bytes, size_t size)
{
    assert(port == 0); const unsigned char *p = bytes; ++writes;
    if (p[0] == 0xaa) {
        if (race) {
            race = false; assert(!pthread_create(&acquisition, NULL, run, NULL));
            while (!waiting) sched_yield();
        }
        if (!absent) { rx[0] = 0x80; rx_at = 0; rx_size = 1; }
    } else if (size == 1 && p[0] == 0xa0) command_pending = true;
    else if (command_pending && size == 8) {
        command_pending = false; rx_at = rx_size = 0;
        if (mode == 1) rx[rx_size++] = 0xf0;
        else if (mode != 2) {
            rx[rx_size++] = 0xa1;
            size_t count = p[0] != 33 ? 1 : p[1] == 2 ? 48 : p[1] == 1 ? 140 : 132;
            if (mode == 3) count--;
            if (mode == 4) count++;
            memcpy(rx + rx_size, payload, count); rx_size += count; rx[rx_size++] = 0xf0;
        }
    }
    return (int)size;
}
int uart_read_bytes(uart_port_t port, void *out, uint32_t size, TickType_t wait)
{
    assert(port == 0);
    if (rx_at == rx_size) { clock_us += (int64_t)wait * 1000; return 0; }
    size_t n = rx_size - rx_at; if (n > size) n = size;
    memcpy(out, rx + rx_at, n); rx_at += n; clock_us += 100; return (int)n;
}
static void started(uint8_t ch) { assert(ch < 4); ++refresh_starts; }
int main(void)
{
    assert(uart_sensors_init() == ESP_OK); clock_us = 0;
    const sensor_inventory_ports_t ports = {.now_us = esp_timer_get_time, .utc_ms = utc, .clock = quality,
        .lock = lock, .unlock = unlock, .yield_ms = yield, .inspect = uart_sensors_inspect};
    sensor_inventory_init(&ports, 123);
    memset(payload, 0, sizeof payload); payload[0] = 1; payload[1] = 4; payload[8] = 1;
    unsigned char out[140]; size_t got;
    assert(uart_sensors_inspect(0, 2, out, 48, &got, 900, started) == SENSOR_READ_OK && got == 48);
    for (mode = 1; mode <= 4; ++mode) {
        int64_t before = clock_us;
        sensor_read_result_t result = uart_sensors_inspect(0, 4, out, 132, &got, 900, started);
        assert(result != SENSOR_READ_OK && clock_us - before <= 1000000);
        unsigned old_mode = mode; mode = 0;
        assert(uart_sensors_inspect(0, 2, out, 48, &got, 900, started) == SENSOR_READ_OK);
        assert(got == 48 && !memcmp(out, payload, 48)); mode = old_mode;
    }
    mode = 0; unsigned previous = refresh_starts;
    uart_sensor_query_fn query = uart_sensors_get_query_fn();
    const unsigned opcodes[] = {20,21,22,23,24,25,26,27,33};
    for (unsigned i = 0; i < sizeof opcodes / sizeof opcodes[0]; ++i) {
        uint8_t cmd[8] = {(uint8_t)opcodes[i], 2}; uart_sensor_response_t response = {0};
        assert(query(0, cmd, NULL, 0, opcodes[i] == 33 ? 48 : 1, &response, 900) == ESP_OK);
        uart_sensor_response_free(&response); assert(refresh_starts == previous);
    }
    assert(uart_sensors_flash_session_begin(0, 100) == ESP_OK);
    assert(uart_sensors_enter_download(0) == ESP_OK); assert(uart_sensors_run_app(0) == ESP_OK);
    uart_sensors_flash_session_end(0); assert(refresh_starts == previous);
    /* Actual refresh-origin path proves its counter is live. */
    sensor_inventory_refresh(); sensor_snapshot_t snapshot; assert(sensor_inventory_snapshot(&snapshot));
    assert(snapshot.refresh.wire_transactions[0] == 1);
    absent = true; race = true; waiting = false; clock_us += 250000;
    sensor_inventory_refresh(); assert(!pthread_join(acquisition, NULL));
    assert(sensor_inventory_snapshot(&snapshot));
    assert(snapshot.refresh.run_start_waits == 1 && snapshot.refresh.run_start_wait_max_us <= 1000000);
    assert(snapshot.busy == 8); sensor_inventory_complete(3);
    assert(writes && refresh_starts == previous);
    puts("UART_SENSOR_OK strict_frames=1 deadline=1 refresh_origin_only=1 shared_bank_race=1");
    return 0;
}

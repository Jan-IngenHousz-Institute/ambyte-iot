/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "cli_sensors.h"
#include "sensor_inventory.h"
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int64_t clock_us;
static bool stalled;
static unsigned char transmitted[SENSOR_SNAPSHOT_MAX + 8];
static size_t transmitted_size;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool logger_start, logger_done;
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(TickType_t ticks) { clock_us += (int64_t)ticks * 1000; sched_yield(); }
static void lock(void) { assert(!pthread_mutex_lock(&mutex)); }
static void unlock(void) { assert(!pthread_mutex_unlock(&mutex)); }
static int64_t utc(void) { return INT64_MAX; }
static uint8_t quality(bool *suspect) { *suspect = false; return 3; }
int usb_serial_jtag_write_bytes(const void *bytes, size_t size, TickType_t ticks)
{
    assert(ticks == 0);
    if (stalled) { logger_start = true; return 0; }
    assert(size <= sizeof transmitted - transmitted_size);
    memcpy(transmitted + transmitted_size, bytes, size); transmitted_size += size;
    return (int)size;
}
/* Any unbounded FILE-output call in the successful/stalled handler is a test
 * failure, even if the current workstation's stdout would happen to drain. */
size_t __wrap_fwrite(const void *b, size_t s, size_t n, FILE *f)
{ (void)b; (void)s; (void)n; (void)f; assert(!"unbounded fwrite"); return 0; }
int __wrap_fputs(const char *b, FILE *f)
{ (void)b; (void)f; assert(!"unbounded fputs"); return 0; }
int __wrap_fflush(FILE *f) { (void)f; assert(!"unbounded fflush"); return 0; }
static void *logger(void *context)
{
    (void)context;
    while (!logger_start) sched_yield();
    flockfile(stdout); logger_done = true; funlockfile(stdout); return NULL;
}
static void put32(unsigned char *p, uint32_t x) { memcpy(p, &x, 4); }
int main(int argc, char **argv)
{
    assert(argc == 2); stalled = !strcmp(argv[1], "stall");
    sensor_inventory_ports_t ports = {.now_us = esp_timer_get_time, .utc_ms = utc,
        .clock = quality, .lock = lock, .unlock = unlock};
    sensor_inventory_init(&ports, UINT64_MAX);
    for (unsigned ch = 0; ch < 4; ++ch) {
        unsigned char fw[48] = {255,255,255}, cal[140], spec[132] = {1};
        fw[8] = (unsigned char)(1+ch); fw[28] = 255;
        memset(cal, 255, sizeof cal); memset(cal, 1, 20);
        for (unsigned i = 20; i < 76; i += 4) put32(cal+i, 0x80000000u);
        for (unsigned i = 100; i < 116; i += 4) put32(cal+i, 0xff7fffffu);
        for (unsigned i = 128; i < 140; i += 4) put32(cal+i, 0xff7fffffu);
        for (unsigned i = 4; i < 132; i += 4) put32(spec+i, 0xff7fffffu);
        assert(sensor_inventory_accept(ch, 2, fw, sizeof fw));
        assert(sensor_inventory_accept(ch, 1, cal, sizeof cal));
        assert(sensor_inventory_accept(ch, 4, spec, sizeof spec));
    }
    pthread_t task;
    if (stalled) assert(!pthread_create(&task, NULL, logger, NULL));
    assert(cli_cmd_sensors(1, NULL) == (stalled ? 1 : 0));
    if (stalled) {
        assert(!pthread_join(task, NULL) && logger_done);
        sensor_snapshot_t s; assert(sensor_inventory_snapshot(&s));
        assert(s.refresh.stdout_hold_max_us == 200000);
        assert(ftrylockfile(stdout) == 0); funlockfile(stdout);
        const char ok[] = "CLI_SENSOR_STALL_OK lock_released=1 bound_us=200000\n";
        assert(write(STDOUT_FILENO, ok, sizeof ok-1) == sizeof ok-1);
    } else {
        assert(transmitted_size > 10000 && transmitted_size <= 12245);
        assert(write(STDOUT_FILENO, transmitted, transmitted_size) == (ssize_t)transmitted_size);
    }
    return 0;
}

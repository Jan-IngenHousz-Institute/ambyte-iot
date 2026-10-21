/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Whole production USB adapter/provider, engine, dispatcher, serializer/cache.
 * The reused runtime harness supplies SDK boundaries and libcrypto only. */
#define SVC_SENSOR_PEER 1
#define main a3_runtime_main
#include "../svc_host/runtime_driver.c"
#undef main

static bool inventory_held, initialized_readonly;
static void inventory_lock(void) { assert(!inventory_held); inventory_held = true; }
static void inventory_unlock(void) { assert(inventory_held); inventory_held = false; }
static int64_t sensor_now(void) { return clock_us; }
static int64_t sensor_utc(void) { return 1792531200000LL + clock_us / 1000; }
static uint8_t sensor_clock(bool *suspect) { *suspect = false; return 3; }
static void sensor_delay(uint32_t ms) { clock_us += (int64_t)ms * 1000; }
void svc_store_fault(const char *point)
{ (void)point; assert(!initialized_readonly); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(!setenv("SVC_TEST_ROOT", argv[1], 1));
    const usb_service_config_t service = {.submit = app_maint_submit, .maintenance_begin = backend_begin,
        .maintenance_end = backend_end, .boot_ready = backend_ready, .worker_stack = backend_stack, .runner_stack = runner_stack};
    assert(usb_service_init(&service) == ESP_OK);
    sensor_inventory_ports_t inventory = {.now_us = sensor_now, .utc_ms = sensor_utc,
        .clock = sensor_clock, .lock = inventory_lock, .unlock = inventory_unlock, .yield_ms = sensor_delay};
    sensor_inventory_init(&inventory, 0x1111111111111111ULL);
    uint8_t fw[48] = {1,4,0}, cal[140] = {0}, spec[132] = {1};
    for (unsigned ch = 0; ch < 4; ++ch) {
        fw[8] = (uint8_t)(ch + 1);
        assert(sensor_inventory_accept(ch, 2, fw, sizeof fw));
        assert(sensor_inventory_accept(ch, 1, cal, sizeof cal));
        assert(sensor_inventory_accept(ch, 4, spec, sizeof spec));
    }
    initialized_readonly = true;
    svc_rx_t rx = {0}; char line[256];
    while (fgets(line, sizeof line, stdin)) {
        if (!strcmp(line, "@advance\n")) { clock_us += 60001000; continue; }
        if (!strcmp(line, "@swap\n")) { fw[8] = 42; assert(sensor_inventory_accept(0, 2, fw, sizeof fw)); continue; }
        for (size_t i = 0; line[i]; ++i) {
            char *body = NULL; uint32_t correlation = 0;
            const char *error = svc_rx_byte(&rx, (unsigned char)line[i], now(NULL), &body, &correlation);
            char json[SVC_JSON_MAX + 1], framed[SVC_FRAME_MAX + 1];
            if (error) svc_protocol_error(&s_engine.protocol, error, json);
            else if (body) (void)svc_engine_dispatch(&s_engine, body, json);
            else continue;
            size_t n = svc_frame(framed, sizeof framed, correlation, json); assert(n);
            assert(fwrite(framed, 1, n, stdout) == n && !fflush(stdout));
            svc_rx_clear(&rx);
        }
    }
    svc_engine_unbind(&s_engine);
    assert(!s_sensor_pin && !inventory_held && !mutex_held);
    fprintf(stderr, "SENSOR_PEER_OK pins_released=1 readonly=1\n");
    return 0;
}

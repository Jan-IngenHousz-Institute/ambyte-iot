#include "cli_sensors.h"
#include "sensor_inventory.h"

#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { unsigned char bytes[128]; size_t used; int64_t deadline; } output_t;
#if CONFIG_AMBYTE_SENSOR_RECOVERY_HIL
int cli_cmd_sensor_recovery_hil(int argc, char **argv)
{
    if (argc != 2 || !argv || !argv[1] || argv[1][0] < '0' || argv[1][0] > '3' || argv[1][1]) {
        printf("Usage: sensors_recovery_hil <0-3>\r\n"); return 1;
    }
    unsigned ch = (unsigned)(argv[1][0] - '0');
    sensor_recovery_result_t result = sensor_inventory_recovery_hil((uint8_t)ch);
    printf("{\"port\":%u,\"recovery\":\"%s\",\"proof\":%s}\r\n", ch,
        result == SENSOR_RECOVERY_PROVEN ? "proven" : result == SENSOR_RECOVERY_FAILED ? "failed" : "refused",
        result == SENSOR_RECOVERY_PROVEN ? "true" : "false");
    return result == SENSOR_RECOVERY_PROVEN ? 0 : 1;
}
#endif
static bool flush(output_t *out)
{
    while (out->used) {
        if (esp_timer_get_time() >= out->deadline) return false;
        int n = usb_serial_jtag_write_bytes(out->bytes, out->used, 0);
        if (n < 0 || (size_t)n > out->used) return false;
        if (n) { memmove(out->bytes, out->bytes + n, out->used - (size_t)n); out->used -= (size_t)n; }
        else vTaskDelay(1);
    }
    return true;
}
static bool sink(void *context, const void *data, size_t size)
{
    output_t *out = context; const unsigned char *bytes = data;
    while (size) {
        if (esp_timer_get_time() >= out->deadline) return false;
        size_t n = sizeof out->bytes - out->used; if (n > size) n = size;
        memcpy(out->bytes + out->used, bytes, n); out->used += n; bytes += n; size -= n;
        if (out->used == sizeof out->bytes && !flush(out)) return false;
    }
    return true;
}
int cli_cmd_sensors(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) { printf("Usage: sensors\r\n"); return 1; }
    sensor_snapshot_t snapshot;
    if (!sensor_inventory_snapshot(&snapshot)) { printf("sensors: unavailable\r\n"); return 1; }
    /* FILE serializes ordinary task logs, but FILE writes themselves can wait
     * forever on the USB VFS. Use bounded driver writes under the same lock;
     * a stalled host must not pin every acquisition logger behind this line. */
    flockfile(stdout);
    const int64_t started = esp_timer_get_time();
    output_t out = {.deadline = started + 200000};
    bool ok = sink(&out, "\r\n", 2) && sensor_snapshot_json(&snapshot, sink, &out) &&
        sink(&out, "\r\r\n", 3) && flush(&out);
    const int64_t elapsed = esp_timer_get_time() - started;
    funlockfile(stdout);
    sensor_inventory_note_stdout(elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
    return ok ? 0 : 1;
}

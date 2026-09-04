/* Bench-only network/memory diagnostics. See docs/bench/RUNBOOK.md. */

#include "bench_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_commands.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#if CONFIG_LWIP_STATS
#include "lwip/stats.h"
#endif

#define BENCH_DIAG_INTERVAL_MS 60000U
#define BENCH_DIAG_STACK       4096U
#define BENCH_DIAG_PRIORITY    1U
#define BENCH_FLOOD_STACK      4096U
#define BENCH_FLOOD_PRIORITY   2U
#define BENCH_FLOOD_HZ_MAX     100U
#define BENCH_FLOOD_BYTES_MIN  32U
#define BENCH_FLOOD_BYTES_MAX  60000U
#define BENCH_FLOOD_SECONDS_MAX (7U * 24U * 60U * 60U)

static const char *TAG = "bench_diag";
static TaskHandle_t s_task;
static TaskHandle_t s_flood_task;

typedef struct {
    uint32_t hz;
    uint32_t bytes;
    uint32_t seconds;
} bench_flood_config_t;

static bench_flood_config_t s_flood_cfg;

static bool parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *out)
{
    if (text == NULL || text[0] == '\0' || out == NULL) return false;
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool build_flood_payload(char *payload, size_t bytes, int64_t sequence)
{
    const int prefix_len = snprintf(payload, bytes + 1U,
                                    "{\"sequence\":%lld,\"padding\":\"",
                                    (long long)sequence);
    static const char suffix[] = "\"}";
    if (prefix_len < 0 || (size_t)prefix_len + sizeof(suffix) - 1U > bytes) {
        return false;
    }
    const size_t pad_len = bytes - (size_t)prefix_len - (sizeof(suffix) - 1U);
    memset(payload + prefix_len, 'x', pad_len);
    memcpy(payload + prefix_len + pad_len, suffix, sizeof(suffix));
    return true;
}

static void bench_flood_task(void *arg)
{
    const bench_flood_config_t cfg = *(const bench_flood_config_t *)arg;
    char *payload = malloc((size_t)cfg.bytes + 1U);
    if (payload == NULL) {
        ESP_LOGE(TAG, "BENCH flood allocation failed: bytes=%u", (unsigned)cfg.bytes);
        s_flood_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const int64_t period_us = 1000000LL / (int64_t)cfg.hz;
    const uint64_t target = (uint64_t)cfg.hz * (uint64_t)cfg.seconds;
    int64_t deadline_us = esp_timer_get_time();
    uint64_t attempted = 0;
    uint64_t stored = 0;
    uint64_t failed = 0;
    int64_t first_id = 0;
    int64_t last_id = 0;

    ESP_LOGW(TAG, "BENCH flood started: rate=%u/s payload=%u bytes duration=%us",
             (unsigned)cfg.hz, (unsigned)cfg.bytes, (unsigned)cfg.seconds);
    while (attempted < target) {
        int64_t measure_id = 0;
        cmd_result_t id_result = cmd_next_measure_id(&measure_id);
        if (id_result.status == ESP_OK &&
            build_flood_payload(payload, cfg.bytes, measure_id)) {
            const int64_t now_ms = (int64_t)time(NULL) * 1000LL;
            const measurement_event_desc_t event = {
                .measure_id = measure_id,
                .channel = "",
                .device = "bench",
                .tag = MEASUREMENT_TAG_MEASUREMENT,
                .cmd_raw = "bench_flood",
                .start_ms = now_ms,
                .end_ms = now_ms,
                .metadata_json = NULL,
                .payload_json = payload,
            };
            cmd_result_t store_result = cmd_store_event(&event);
            if (store_result.status == ESP_OK) {
                if (first_id == 0) first_id = measure_id;
                last_id = measure_id;
                stored++;
                if (stored % 100U == 0U) {
                    ESP_LOGW(TAG, "BENCH flood progress: stored=%llu failed=%llu first_id=%lld last_id=%lld",
                             (unsigned long long)stored, (unsigned long long)failed,
                             (long long)first_id,
                             (long long)measure_id);
                }
            } else {
                failed++;
                if (failed == 1U || failed % 100U == 0U) {
                    ESP_LOGE(TAG, "BENCH flood store failed: count=%llu id=%lld %s",
                             (unsigned long long)failed, (long long)measure_id,
                             store_result.message);
                }
            }
        } else {
            failed++;
            if (failed == 1U || failed % 100U == 0U) {
                ESP_LOGE(TAG, "BENCH flood id/payload failed: count=%llu %s",
                         (unsigned long long)failed, id_result.message);
            }
        }
        attempted++;

        deadline_us += period_us;
        int64_t delay_us = deadline_us - esp_timer_get_time();
        if (delay_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((delay_us + 999LL) / 1000LL)));
        } else if (delay_us < -period_us) {
            deadline_us = esp_timer_get_time();
        }
    }

    ESP_LOGW(TAG, "BENCH flood complete: attempted=%llu stored=%llu failed=%llu first_id=%lld last_id=%lld",
             (unsigned long long)attempted,
             (unsigned long long)stored, (unsigned long long)failed,
             (long long)first_id, (long long)last_id);
    free(payload);
    s_flood_task = NULL;
    vTaskDelete(NULL);
}

static int bench_flood_command(int argc, char **argv)
{
    bench_flood_config_t cfg;
    if (argc != 4 ||
        !parse_u32(argv[1], 1U, BENCH_FLOOD_HZ_MAX, &cfg.hz) ||
        !parse_u32(argv[2], BENCH_FLOOD_BYTES_MIN, BENCH_FLOOD_BYTES_MAX, &cfg.bytes) ||
        !parse_u32(argv[3], 1U, BENCH_FLOOD_SECONDS_MAX, &cfg.seconds)) {
        printf("usage: bench_flood <hz:1-%u> <bytes:%u-%u> <seconds:1-%u>\r\n",
               (unsigned)BENCH_FLOOD_HZ_MAX,
               (unsigned)BENCH_FLOOD_BYTES_MIN,
               (unsigned)BENCH_FLOOD_BYTES_MAX,
               (unsigned)BENCH_FLOOD_SECONDS_MAX);
        return 1;
    }
    if (s_flood_task != NULL) {
        printf("bench_flood already running\r\n");
        return 1;
    }

    s_flood_cfg = cfg;
    if (xTaskCreate(bench_flood_task, "bench_flood", BENCH_FLOOD_STACK,
                    &s_flood_cfg, BENCH_FLOOD_PRIORITY, &s_flood_task) != pdPASS) {
        s_flood_task = NULL;
        printf("bench_flood: task creation failed\r\n");
        return 1;
    }
    printf("bench_flood started\r\n");
    return 0;
}

static const esp_console_cmd_t s_bench_flood_cmd = {
    .command = "bench_flood",
    .help = "bench_flood <hz> <bytes> <seconds>  store synthetic events (bench only)",
    .hint = NULL,
    .func = bench_flood_command,
};

/* lwIP owns the upper CONFIG_LWIP_MAX_SOCKETS descriptors. F_GETFL is one of
 * lwip_fcntl's two supported commands and safely acquires/releases a socket
 * reference, so a concurrent MQTT reconnect cannot leave us dereferencing a
 * stale private socket slot. The result is a point-in-time open-socket count. */
static unsigned open_socket_count(void)
{
    unsigned open = 0;
    const int first = LWIP_SOCKET_OFFSET;
    const int last = first + CONFIG_LWIP_MAX_SOCKETS;

    for (int fd = first; fd < last; ++fd) {
        errno = 0;
        if (lwip_fcntl(fd, F_GETFL, 0) >= 0) {
            ++open;
        }
    }
    return open;
}

static void log_heap_caps(const char *name, uint32_t caps)
{
    ESP_LOGW(TAG, "BENCH heap=%s free=%u largest=%u min_free=%u",
             name,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

static void report_once(void)
{
    ESP_LOGW(TAG, "BENCH sample begin sockets_open=%u sockets_max=%u",
             open_socket_count(), (unsigned)CONFIG_LWIP_MAX_SOCKETS);

    log_heap_caps("INTERNAL", MALLOC_CAP_INTERNAL);
    log_heap_caps("DMA", MALLOC_CAP_DMA);
    log_heap_caps("SPIRAM", MALLOC_CAP_SPIRAM);

#if CONFIG_LWIP_STATS
    /* stats_display() is the authoritative full IDF/lwIP dump. It writes to the
     * console through LWIP_PLATFORM_DIAG. The compact WARN summaries bracketing
     * it are also retained by this firmware's WARN/ERROR-only SD logger. */
    ESP_LOGW(TAG,
             "BENCH lwip tcp{xmit=%u recv=%u drop=%u memerr=%u err=%u} "
             "link{xmit=%u recv=%u drop=%u memerr=%u} "
             "sys{sem=%u/%u semerr=%u mbox=%u/%u mboxerr=%u}",
             (unsigned)lwip_stats.tcp.xmit,
             (unsigned)lwip_stats.tcp.recv,
             (unsigned)lwip_stats.tcp.drop,
             (unsigned)lwip_stats.tcp.memerr,
             (unsigned)lwip_stats.tcp.err,
             (unsigned)lwip_stats.link.xmit,
             (unsigned)lwip_stats.link.recv,
             (unsigned)lwip_stats.link.drop,
             (unsigned)lwip_stats.link.memerr,
             (unsigned)lwip_stats.sys.sem.used,
             (unsigned)lwip_stats.sys.sem.max,
             (unsigned)lwip_stats.sys.sem.err,
             (unsigned)lwip_stats.sys.mbox.used,
             (unsigned)lwip_stats.sys.mbox.max,
             (unsigned)lwip_stats.sys.mbox.err);
    stats_display();
#else
    ESP_LOGW(TAG, "BENCH lwIP stats unavailable (CONFIG_LWIP_STATS is disabled)");
#endif

    /* IDF 5.5's public Wi-Fi API exposes buffer counters only as a textual
     * diagnostic dump; there is no public structured buffer-stat getter. */
    const esp_err_t wifi_dump = esp_wifi_statis_dump(WIFI_STATIS_BUFFER);
    ESP_LOGW(TAG, "BENCH wifi_buffer_dump=%s sample end", esp_err_to_name(wifi_dump));
}

static void bench_diag_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        report_once();
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(BENCH_DIAG_INTERVAL_MS));
    }
}

esp_err_t bench_diag_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

#if CONFIG_AMBYTE_BENCH_WIFI_PS_NONE
    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGW(TAG, "BENCH Wi-Fi power save override: WIFI_PS_NONE (%s)",
             esp_err_to_name(ps_err));
    if (ps_err != ESP_OK) {
        return ps_err;
    }
#endif

    const esp_err_t console_err = esp_console_cmd_register(&s_bench_flood_cmd);
    if (console_err != ESP_OK) {
        ESP_LOGE(TAG, "bench_flood registration failed: %s", esp_err_to_name(console_err));
        return console_err;
    }

    if (xTaskCreate(bench_diag_task, "bench_diag", BENCH_DIAG_STACK, NULL,
                    BENCH_DIAG_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "BENCH diagnostics enabled: interval=%us",
             (unsigned)(BENCH_DIAG_INTERVAL_MS / 1000U));
    return ESP_OK;
}

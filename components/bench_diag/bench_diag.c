/* Bench-only network/memory diagnostics. See docs/bench/RUNBOOK.md. */

#include "bench_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#if CONFIG_LWIP_STATS
#include "lwip/stats.h"
#endif

#define BENCH_DIAG_INTERVAL_MS 60000U
#define BENCH_DIAG_STACK       4096U
#define BENCH_DIAG_PRIORITY    1U

static const char *TAG = "bench_diag";
static TaskHandle_t s_task;

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

    if (xTaskCreate(bench_diag_task, "bench_diag", BENCH_DIAG_STACK, NULL,
                    BENCH_DIAG_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "BENCH diagnostics enabled: interval=%us",
             (unsigned)(BENCH_DIAG_INTERVAL_MS / 1000U));
    return ESP_OK;
}

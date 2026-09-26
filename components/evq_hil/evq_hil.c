/* `evq_hil` — on-device SD-overflow verification controls
 * (CONFIG_AMBYTE_EVQ_HIL only; docs/evq-sd-overflow-hil-contract.md).
 *
 * Every subcommand prints machine-parseable lines (EVQ_* / HIL_*). The fill
 * stores through the PRODUCTION path (cmd_store_event -> event_log_store_event)
 * and prints EVQ_ACC only after that returned ESP_OK (= fsync'd). Nothing here
 * deletes anything; the inventories are read-only. */
#include "evq_hil.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "device_commands.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "event_log.h"
#include "event_log_hil.h"
#include "evq_hil_payload.h"
#include "evq_index.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sd_card.h"

static const char *TAG = "evq_hil";

/* ── publish gate override ───────────────────────────────────────────────
 * Strong definition of sync_runner's weak sync_runner_is_allowed(): HOLD
 * keeps every record PENDING (delivery closed during the fill), FORCE opens
 * the power gate only (the sensor hold still applies), AUTO is exactly the
 * production rule. The override is RTC-retained (event_log_hil). */
bool sync_runner_is_allowed(void)
{
    evq_hil_gate_t g = event_log_hil_gate();
    if (g == EVQ_HIL_GATE_HOLD) return false;
#if AMBYTE_PUBLISH_GATE_LEGACY
    bool sensor_gate_open = !device_commands_measurement_active();
#else
    bool sensor_gate_open = !device_commands_publish_hold_active();
#endif
    if (g == EVQ_HIL_GATE_FORCE) return sensor_gate_open;
    return sensor_gate_open && device_commands_publish_power_ok();
}

/* ── park hooks (production routines in app_main) ── */
static void (*s_park)(void);
static void (*s_unpark)(void);

void evq_hil_set_park_hooks(void (*park)(void), void (*unpark)(void))
{
    s_park = park;
    s_unpark = unpark;
}

static void park_task(void *arg)
{
    bool park = (bool)(uintptr_t)arg;
    int64_t t0 = esp_timer_get_time();
    if (park && s_park) s_park();
    else if (!park && s_unpark) s_unpark();
    printf("HIL_PARK %s done us=%lld dur_us=%lld\n", park ? "park" : "unpark",
           (long long)esp_timer_get_time(), (long long)(esp_timer_get_time() - t0));
    vTaskDelete(NULL);
}

/* ── helpers ── */
static void hex32(const uint8_t d[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
    out[64] = '\0';
}

static bool parse_u64(const char *s, uint64_t *out)
{
    if (s == NULL || *s == '\0') return false;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = v;
    return true;
}

/* ── fill ── */
typedef struct {
    char     run[48];
    uint32_t k_start, n, bytes, hz;
    bool     cont;
} fill_cfg_t;

static fill_cfg_t s_fill;
static TaskHandle_t s_fill_task;

static void fill_task(void *arg)
{
    (void)arg;
    const fill_cfg_t c = s_fill;
    size_t cap = c.bytes + 128;
    char *payload = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (payload == NULL) payload = malloc(cap);
    char cmd[64];
    snprintf(cmd, sizeof cmd, "evq_hil %s", c.run);
    int64_t next0 = 0;
    (void)event_log_db_stats(NULL, NULL, NULL, &next0);
    printf("EVQ_BEGIN %s %u %u %u %lld\n", c.run, (unsigned)c.k_start, (unsigned)c.n, (unsigned)c.bytes,
           (long long)next0);
    uint32_t acc = 0, ref = 0, k = c.k_start;
    TickType_t wake = xTaskGetTickCount();
    const TickType_t period = c.hz ? pdMS_TO_TICKS(1000 / c.hz) : 0;
    for (uint32_t i = 0; payload != NULL && i < c.n; i++, k++) {
        size_t plen = evq_hil_payload(c.run, k, c.bytes, payload, cap);
        int64_t id = 0;
        cmd_result_t ir = cmd_next_measure_id(&id);
        if (plen == 0 || ir.status != ESP_OK) {
            printf("EVQ_REF %s %u - %s id_or_payload\n", c.run, (unsigned)k, esp_err_to_name(ir.status));
            ref++;
            if (!c.cont) break;
            continue;
        }
        const int64_t now_ms = (int64_t)time(NULL) * 1000LL;
        const measurement_event_desc_t ev = {
            .measure_id = id, .channel = "", .device = "evq_hil", .tag = MEASUREMENT_TAG_MEASUREMENT,
            .cmd_raw = cmd, .start_ms = now_ms, .end_ms = now_ms, .metadata_json = NULL, .payload_json = payload,
        };
        cmd_result_t sr = cmd_store_event(&ev);
        if (sr.status == ESP_OK) {
            uint8_t ls[32] = { 0 }, ps[32];
            uint32_t lbytes = 0;
            bool have = event_log_hil_line_sha(id, ls, &lbytes);
            mbedtls_sha256((const unsigned char *)payload, plen, ps, 0);
            char lh[65], ph[65];
            hex32(ls, lh);
            hex32(ps, ph);
            printf("EVQ_ACC %s %u %lld %lld %lld %u %s %s\n", c.run, (unsigned)k, (long long)id,
                   (long long)now_ms, (long long)now_ms, (unsigned)lbytes, have ? lh : "-", ph);
            acc++;
        } else {
            evlog_health_t h;
            const char *why = "-";
            if (event_log_health(&h) == ESP_OK) why = event_log_blocked_reason_name(h.blocked_reason);
            printf("EVQ_REF %s %u %lld %s %s\n", c.run, (unsigned)k, (long long)id, esp_err_to_name(sr.status), why);
            ref++;
            if (!c.cont) break;
        }
        if (period) vTaskDelayUntil(&wake, period);
        else taskYIELD();
    }
    int64_t next1 = 0;
    (void)event_log_db_stats(NULL, NULL, NULL, &next1);
    printf("EVQ_END %s %u %u %u %lld\n", c.run, (unsigned)(k ? k - 1 : 0), (unsigned)acc, (unsigned)ref,
           (long long)next1);
    free(payload);
    s_fill_task = NULL;
    vTaskDelete(NULL);
}

/* ── SD inventory (read-only) ── */
typedef struct {
    char  *line;
    size_t line_cap;
    char  *buf;
    bool   lines;
    unsigned files, missing;
} inv_t;

static void inv_file(inv_t *iv, const char *path, long size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { iv->missing++; printf("EVQ_FMISS %s\n", path); return; }
    mbedtls_sha256_context fc;
    mbedtls_sha256_init(&fc);
    mbedtls_sha256_starts(&fc, 0);
    uint32_t crc = 0;
    size_t ll = 0, r;
    unsigned lno = 0;
    long total = 0;
    while ((r = fread(iv->buf, 1, 4096, f)) > 0) {
        mbedtls_sha256_update(&fc, (const unsigned char *)iv->buf, r);
        crc = evq_crc32(crc, iv->buf, r);
        total += (long)r;
        if (!iv->lines) continue;
        for (size_t k = 0; k < r; k++) {
            if (ll < iv->line_cap) iv->line[ll] = iv->buf[k];
            ll++;
            if (iv->buf[k] == '\n') {
                uint8_t d[32];
                char hx[65];
                mbedtls_sha256((const unsigned char *)iv->line, ll <= iv->line_cap ? ll : iv->line_cap, d, 0);
                hex32(d, hx);
                lno++;
                printf("EVQ_FL %s %u %lld %s %u\n", path, lno, strtoll(iv->line, NULL, 10), hx, (unsigned)ll);
                ll = 0;
            }
        }
    }
    bool err = ferror(f) != 0;
    fclose(f);
    uint8_t d[32];
    char hx[65];
    mbedtls_sha256_finish(&fc, d);
    mbedtls_sha256_free(&fc);
    hex32(d, hx);
    if (iv->lines && ll > 0) printf("EVQ_FT %s %u %u\n", path, lno + 1, (unsigned)ll);
    if (err || total != size) { iv->missing++; printf("EVQ_FSHORT %s %ld %ld\n", path, total, size); return; }
    printf("EVQ_FF %s %ld %s %08" PRIx32 "\n", path, size, hx, crc);
    iv->files++;
}

static void inv_dir(inv_t *iv, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (d == NULL) { printf("EVQ_DMISS %s\n", dir); return; }
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) { printf("EVQ_FMISS %s\n", path); iv->missing++; continue; }
        if (S_ISDIR(st.st_mode)) {
            printf("EVQ_DIR %s\n", path);
            if (depth < 6) inv_dir(iv, path, depth + 1);
            continue;
        }
        if (strncmp(path, "/sdcard/logs", 12) == 0) {
            printf("EVQ_FN %s %ld\n", path, (long)st.st_size);    /* logs: name + size only */
            continue;
        }
        inv_file(iv, path, (long)st.st_size);
    }
    closedir(d);
}

static int do_sd_inv(const char *dir, bool lines)
{
    if (!event_log_hil_keeper_paused() && !event_log_hil_held()) {
        printf("EVQ_FERR keeper_running (evq_hil keeper pause first)\n");
        return 1;
    }
    if (!sdcard_io_begin()) { printf("EVQ_FERR sd_unavailable\n"); return 1; }
    inv_t iv = { .line_cap = 70 * 1024, .lines = lines };
    iv.line = heap_caps_malloc(iv.line_cap, MALLOC_CAP_SPIRAM);
    iv.buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (iv.line == NULL || iv.buf == NULL) {
        free(iv.line); free(iv.buf);
        sdcard_io_end();
        printf("EVQ_FERR no_mem\n");
        return 1;
    }
    uint64_t total = 0, freeb = 0;
    (void)esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb);
    printf("EVQ_SDSNAP %s cid=%08" PRIx32 " total=%llu free=%llu us=%lld\n", dir, sdcard_card_serial(),
           (unsigned long long)total, (unsigned long long)freeb, (long long)esp_timer_get_time());
    inv_dir(&iv, dir, 0);
    printf("EVQ_SDEND %u %u\n", iv.files, iv.missing);
    free(iv.line); free(iv.buf);
    sdcard_io_end();
    return iv.missing == 0 ? 0 : 1;
}

/* ── stacks ── */
static void do_stacks(void)
{
    static const char *const names[] = { "sd_keeper", "sync_runner", "sync_wdog", "mqtt_task", "sched_runner",
                                         "maint", "pwr_guard", "sd_monitor", "sd_logger", "status_hb",
                                         "console_repl", "evq_fill", "main" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t == NULL) continue;
        printf("EVQ_STACK %s %u\n", names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
    }
    printf("EVQ_STACKEND heap_internal=%u heap_psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ── command ── */
static void usage(void)
{
    printf("evq_hil fill <run> <k_start> <n> <bytes> <hz> [-c] | flash_inv | sd_inv <dir> [lines] | index |\n"
           "        io [drain] | claims | cursor | state | stacks | gate <hold|auto|force> | sd_release |\n"
           "        keeper <pause|run> | sd_park | sd_unpark | fault <point> <reset|reset_inside|eio|enospc|off> [nth] |\n"
           "        reserve <bytes|off> | cid <hex|off> | boot_slot <ota_0|ota_1>\n");
}

static int evq_hil_cmd(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    const char *sub = argv[1];
    if (strcmp(sub, "fill") == 0) {
        uint64_t k0, n, b, hz;
        if (argc < 7 || !parse_u64(argv[3], &k0) || !parse_u64(argv[4], &n) || !parse_u64(argv[5], &b) ||
            !parse_u64(argv[6], &hz) || n == 0 || b < 16 || b > 60000 || hz > 100 || strlen(argv[2]) >= sizeof s_fill.run) {
            usage();
            return 1;
        }
        if (s_fill_task != NULL) { printf("HIL_ERR fill_running\n"); return 1; }
        snprintf(s_fill.run, sizeof s_fill.run, "%s", argv[2]);
        s_fill.k_start = (uint32_t)k0;
        s_fill.n = (uint32_t)n;
        s_fill.bytes = (uint32_t)b;
        s_fill.hz = (uint32_t)hz;
        s_fill.cont = argc >= 8 && strcmp(argv[7], "-c") == 0;
        if (xTaskCreate(fill_task, "evq_fill", 6144, NULL, 3, &s_fill_task) != pdPASS) {
            s_fill_task = NULL;
            printf("HIL_ERR task\n");
            return 1;
        }
        printf("HIL_OK fill started\n");
        return 0;
    }
    if (strcmp(sub, "flash_inv") == 0) return event_log_hil_flash_inv() == ESP_OK ? 0 : 1;
    if (strcmp(sub, "sd_inv") == 0 && argc >= 3) return do_sd_inv(argv[2], argc >= 4 && strcmp(argv[3], "lines") == 0);
    if (strcmp(sub, "index") == 0) return event_log_hil_index_dump() == ESP_OK ? 0 : 1;
    if (strcmp(sub, "io") == 0) return event_log_hil_io_dump(argc >= 3 && strcmp(argv[2], "drain") == 0) == ESP_OK ? 0 : 1;
    if (strcmp(sub, "claims") == 0) return event_log_hil_claims_dump() == ESP_OK ? 0 : 1;
    if (strcmp(sub, "cursor") == 0) return event_log_hil_cursor_dump() == ESP_OK ? 0 : 1;
    if (strcmp(sub, "state") == 0) return event_log_hil_state_dump() == ESP_OK ? 0 : 1;
    if (strcmp(sub, "stacks") == 0) { do_stacks(); return 0; }
    if (strcmp(sub, "gate") == 0 && argc >= 3) {
        evq_hil_gate_t g = strcmp(argv[2], "hold") == 0 ? EVQ_HIL_GATE_HOLD
                         : strcmp(argv[2], "force") == 0 ? EVQ_HIL_GATE_FORCE
                         : strcmp(argv[2], "auto") == 0 ? EVQ_HIL_GATE_AUTO : (evq_hil_gate_t)99;
        if ((int)g == 99) { usage(); return 1; }
        event_log_hil_set_gate(g);
        printf("HIL_OK gate=%u\n", (unsigned)g);
        return 0;
    }
    if (strcmp(sub, "sd_release") == 0) { event_log_hil_release(); printf("HIL_OK released\n"); return 0; }
    if (strcmp(sub, "keeper") == 0 && argc >= 3) {
        event_log_hil_keeper_pause(strcmp(argv[2], "pause") == 0);
        printf("HIL_OK keeper_paused=%d\n", event_log_hil_keeper_paused() ? 1 : 0);
        return 0;
    }
    if (strcmp(sub, "sd_park") == 0 || strcmp(sub, "sd_unpark") == 0) {
        bool park = strcmp(sub, "sd_park") == 0;
        if ((park && s_park == NULL) || (!park && s_unpark == NULL)) { printf("HIL_ERR no_hook\n"); return 1; }
        if (xTaskCreate(park_task, "evq_park", 12288, (void *)(uintptr_t)park, 2, NULL) != pdPASS) {
            printf("HIL_ERR task\n");
            return 1;
        }
        printf("HIL_OK %s started\n", sub);
        return 0;
    }
    if (strcmp(sub, "fault") == 0 && argc >= 4) {
        uint64_t nth = 1;
        if (argc >= 5 && !parse_u64(argv[4], &nth)) { usage(); return 1; }
        esp_err_t e = event_log_hil_arm(argv[2], argv[3], (unsigned)nth);
        printf("HIL_%s fault %s %s %u\n", e == ESP_OK ? "OK" : "ERR", argv[2], argv[3], (unsigned)nth);
        return e == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "reserve") == 0 && argc >= 3) {
        uint64_t v = 0;
        if (strcmp(argv[2], "off") != 0 && !parse_u64(argv[2], &v)) { usage(); return 1; }
        event_log_hil_set_reserve(v);
        printf("HIL_OK reserve=%llu\n", (unsigned long long)event_log_hil_reserve());
        return 0;
    }
    if (strcmp(sub, "cid") == 0 && argc >= 3) {
        uint64_t v = 0;
        if (strcmp(argv[2], "off") != 0 && !parse_u64(argv[2], &v)) { usage(); return 1; }
        event_log_hil_set_cid((uint32_t)v);
        printf("HIL_OK cid_override=%08" PRIx32 "\n", event_log_hil_cid());
        return 0;
    }
    if (strcmp(sub, "boot_slot") == 0 && argc >= 3) {
        esp_partition_subtype_t st = strcmp(argv[2], "ota_0") == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                   : strcmp(argv[2], "ota_1") == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                   : ESP_PARTITION_SUBTYPE_ANY;
        if (st == ESP_PARTITION_SUBTYPE_ANY) { usage(); return 1; }
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, st, NULL);
        esp_err_t e = p ? esp_ota_set_boot_partition(p) : ESP_ERR_NOT_FOUND;
        printf("HIL_%s boot_slot %s %s\n", e == ESP_OK ? "OK" : "ERR", argv[2], esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    usage();
    return 1;
}

void evq_hil_register(void)
{
    const esp_console_cmd_t c = {
        .command = "evq_hil",
        .help = "SD-overflow verification controls (test build only)",
        .func = evq_hil_cmd,
    };
    esp_err_t e = esp_console_cmd_register(&c);
    ESP_LOGW(TAG, "evq_hil verification controls %s (test build — never a release)",
             e == ESP_OK ? "registered" : esp_err_to_name(e));
}

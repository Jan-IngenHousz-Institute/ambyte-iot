/* AMBIT update worker under brownout protection (review of PR #103).
 *
 * Runs the PRODUCTION power_blocks_write / ambit_do_ota / ambit_do_flash /
 * ambit_ota_run, extracted verbatim from components/ambit_ota/ambit_ota.c by
 * tests/test_power_protect.py into ambit-worker-production.inc, against fakes
 * for everything they call. Protection switches on at a chosen moment: during
 * the ack flush, the fleet spread, the download / release fetch, or while a
 * channel is being written (which must finish), and the worker must not start
 * any LATER write.
 *
 * usage: ambit_worker <scenario>   (exit 0 = PASS; prints REPORT lines) */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define TAG "ambit_ota"
static const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
/* Format-checked and evaluated, so the production log lines compile as written. */
__attribute__((format(printf, 2, 3))) static void host_log(const char *tag, const char *fmt, ...)
{ (void)tag; (void)fmt; }
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
#define pdMS_TO_TICKS(ms) (ms)
#define UART_SENSOR_NUM_CHANNELS 4
#define AMBIT_OTA_CH_ALL 0xFFu
#define AMBIT_OTA_URL_MAX 256
#define AMBIT_OTA_ID_MAX 64
#define AMBIT_OP_OTA 0
#define AMBIT_OP_VERSIONS 1
#define AMBIT_OP_PROBE 2
#define AMBIT_OP_FLASH 3
#define AMBIT_OP_ATTACH 4
#define KEY_APPLIED "applied"
#define KEY_APPLIED_FLASH "applied_fl"

typedef struct {
    uint8_t op, channel;
    bool    fleet_spread;
    uint8_t ver[3];
    char    url[AMBIT_OTA_URL_MAX];
    char    manifest_url[AMBIT_OTA_URL_MAX];
    char    id[AMBIT_OTA_ID_MAX];
} ambit_ota_req_t;
typedef struct { esp_err_t status; char message[32]; } cmd_result_t;
typedef struct { char path[96]; size_t len; } ambit_stage_t;
typedef struct { const char *why; unsigned downloaded, reused, removed; unsigned long bytes; } ambit_fw_fetch_result_t;
typedef struct { int unused; } ambit_fw_http_t;
typedef struct { bool connected; } ambit_flash_probe_result_t;
typedef struct { bool ok; int chip; int regions_written; size_t total_bytes; } ambit_flash_image_result_t;
typedef enum { PP_STATE_NORMAL = 0, PP_STATE_NO_FLASH = 1, PP_STATE_HOLD = 2 } pp_state_t;
typedef enum { POWER_PROTECT_SKIP_FLASH_RUN = 0, POWER_PROTECT_SKIP_AMBIT_RUN, POWER_PROTECT_SKIP_OTA } power_protect_skip_t;

/* ── the scenario and what the worker did ─────────────────────────────── */
static const char *sc;
static bool pp_on;
static unsigned skips, suspends, resumes, comms_down, comms_up, maint_depth, latches, fail_notes, delays;
static unsigned downloads, fetches, writes[UART_SENSOR_NUM_CHANNELS];
static bool is(const char *s) { return strcmp(sc, s) == 0; }

bool power_protect_blocks_flash(void) { return pp_on; }
pp_state_t power_protect_state(void) { return pp_on ? PP_STATE_NO_FLASH : PP_STATE_NORMAL; }
const char *pp_state_name(pp_state_t s) { return s == PP_STATE_NO_FLASH ? "no_flash" : "normal"; }
void power_protect_note_skip(power_protect_skip_t w) { assert(w == POWER_PROTECT_SKIP_OTA); ++skips; }

static void vTaskDelay(uint32_t t) { (void)t; if (++delays == 1 && is("flash_flip_during_ack")) pp_on = true; }
static void workload_suspend(void) { ++suspends; }
static void workload_resume(void) { ++resumes; }
static void comms_suspend(void) { ++comms_down; }
static void comms_resume(void) { ++comms_up; }
static bool maintenance_begin(void) { ++maint_depth; return true; }
static void maintenance_end(void) { assert(maint_depth > 0); --maint_depth; }
static bool is_connected(void) { return true; }
static struct {
    void (*workload_suspend)(void); void (*workload_resume)(void);
    void (*comms_suspend)(void); void (*comms_resume)(void);
    bool (*maintenance_begin)(void); void (*maintenance_end)(void);
    bool (*is_connected)(void);
} ambit_cfg = { workload_suspend, workload_resume, comms_suspend, comms_resume,
                maintenance_begin, maintenance_end, is_connected };
static const ambit_fw_http_t s_fetch_http;

static void report_as(const char *type, const char *state, uint8_t ch, const char *id, const char *detail)
{
    printf("REPORT %s %s ch=%u id=%s detail=%s\n", type, state, (unsigned)ch, id, detail ? detail : "-");
}
static void report(const char *state, uint8_t ch, const char *id) { report_as("ambit_ota_status", state, ch, id, NULL); }
static void report_detail(const char *state, uint8_t ch, const char *id, const char *d) { report_as("ambit_ota_status", state, ch, id, d); }
static void report_busy(const ambit_ota_req_t *r) { (void)r; assert(0); }
static void latch_set(const char *key, const char *id) { (void)key; (void)id; ++latches; }
static bool already_applied(const char *key, const char *id) { (void)key; (void)id; return false; }
static void flash_fail_note(const char *id) { (void)id; ++fail_notes; }
static void flash_fail_clear(void) {}
static void wait_for_fleet_slot(void) { if (is("ota_flip_during_spread")) pp_on = true; }
static void ambit_quiesce_for_idle(void) { workload_suspend(); }
static bool sdcard_is_mounted(void) { return true; }
static esp_err_t sdcard_mount(void) { return ESP_OK; }
static bool sdcard_io_begin(void) { return true; }
static void sdcard_io_end(void) {}
static esp_err_t ambit_stage_remove(const ambit_stage_t *s) { (void)s; return ESP_OK; }
static esp_err_t http_get_to_stage(const char *url, ambit_stage_t *st)
{
    (void)url; ++downloads;
    snprintf(st->path, sizeof st->path, "/sdcard/stage.bin"); st->len = 4096;
    if (is("ota_flip_during_download")) pp_on = true;
    return ESP_OK;
}
static cmd_result_t cmd_uart_ping(uint8_t ch, bool *connected) { (void)ch; *connected = true; return (cmd_result_t){ESP_OK, ""}; }
static bool ambit_ota_one(uint8_t ch, const ambit_stage_t *st)
{
    (void)st; ++writes[ch];
    if (ch == 1 && is("ota_flip_mid_sweep")) pp_on = true;   /* begins while ch1 streams */
    return true;
}
static esp_err_t ambit_fw_fetch(const char *root, const char *url, uint8_t a, uint8_t b, uint8_t c,
                                const ambit_fw_http_t *http, ambit_fw_fetch_result_t *out)
{
    (void)root; (void)url; (void)a; (void)b; (void)c; (void)http; ++fetches;
    *out = (ambit_fw_fetch_result_t){ "ok", 4, 0, 0, 1000 };
    if (is("flash_flip_during_fetch")) pp_on = true;
    return ESP_OK;
}
static esp_err_t ambit_flash_probe(uint8_t ch, ambit_flash_probe_result_t *out) { (void)ch; out->connected = true; return ESP_OK; }
static esp_err_t ambit_flash_image(uint8_t ch, const char *dir, uint32_t baud, ambit_flash_image_result_t *out)
{
    (void)dir; (void)baud; ++writes[ch];
    *out = (ambit_flash_image_result_t){ true, 3, 4, 1000 };
    if (ch == 0 && is("flash_flip_mid_sweep")) pp_on = true;  /* begins while ch0 flashes */
    return ESP_OK;
}
static void cmd_ambit_device_info_invalidate(uint8_t ch) { (void)ch; }
static void ambit_do_versions(const char *id) { (void)id; assert(0); }
static void ambit_do_probe(const ambit_ota_req_t *r) { (void)r; assert(0); }
static void ambit_do_attach(void) { assert(0); }
static void ambit_attach_gate_checker_done(void) {}

#define s_cfg ambit_cfg
#include "ambit-worker-production.inc"
#undef s_cfg

static void run(uint8_t op, uint8_t ch, bool spread, const char *manifest)
{
    ambit_ota_req_t *r = calloc(1, sizeof *r);
    r->op = op; r->channel = ch; r->fleet_spread = spread;
    snprintf(r->url, sizeof r->url, op == AMBIT_OP_FLASH ? "/sdcard/ambit_fw/1.4.0" : "https://example.test/a.bin");
    snprintf(r->manifest_url, sizeof r->manifest_url, "%s", manifest);
    snprintf(r->id, sizeof r->id, "job-1");
    ambit_ota_run(r);   /* frees r */
    assert(maint_depth == 0);
    assert(comms_down == comms_up && suspends == resumes);   /* everything suspended came back */
}

static unsigned total_writes(void) { return writes[0] + writes[1] + writes[2] + writes[3]; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    sc = argv[1];
    if (is("ota_protected_at_entry")) {
        pp_on = true;
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, true, "");
        assert(downloads == 0 && total_writes() == 0 && suspends == 0 && comms_down == 0 && skips == 1);
    } else if (is("ota_flip_during_spread")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, true, "");
        assert(downloads == 0 && total_writes() == 0 && suspends == 0 && comms_down == 0 && skips == 1);
    } else if (is("ota_flip_during_download")) {
        run(AMBIT_OP_OTA, 2, false, "");
        assert(downloads == 1 && total_writes() == 0 && latches == 0 && skips == 1);
    } else if (is("ota_flip_mid_sweep")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, false, "");
        /* ch0 and ch1 (started before protection) finished; ch2, ch3 never started. */
        assert(writes[0] == 1 && writes[1] == 1 && writes[2] == 0 && writes[3] == 0);
        assert(latches == 0 && skips == 1);
    } else if (is("flash_flip_during_ack")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "");
        assert(fetches == 0 && total_writes() == 0 && suspends == 0 && comms_down == 0 && skips == 1);
        assert(fail_notes == 0);
    } else if (is("flash_flip_during_fetch")) {
        run(AMBIT_OP_FLASH, 1, false, "https://example.test/manifest.json");
        assert(fetches == 1 && total_writes() == 0 && latches == 0 && fail_notes == 0 && skips == 1);
    } else if (is("flash_flip_mid_sweep")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "");
        assert(writes[0] == 1 && writes[1] == 0 && writes[2] == 0 && writes[3] == 0);
        assert(latches == 0 && fail_notes == 0 && skips == 1);
    } else if (is("ota_unprotected")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, true, "");
        assert(total_writes() == 4 && latches == 1 && skips == 0);
    } else if (is("flash_unprotected")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "");
        assert(total_writes() == 4 && latches == 1 && skips == 0);
    } else {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }
    printf("PASS %s\n", sc);
    return 0;
}

/* AMBIT update paths under brownout protection (review rounds 1 and 2 of PR #103).
 *
 * Runs PRODUCTION code extracted verbatim by tests/test_power_protect.py into
 * ambit-worker-production.inc:
 *   components/ambit_flash/ambit_flash.c   the region table, boot_fail_*,
 *       flash_power_allows_write, ambit_flash_image_gated,
 *       ambit_flash_attach_flash, fw_matches;
 *   components/ambit_ota/ambit_ota.c   power_note_refusal, power_blocks_write,
 *       power_allows_write, ambit_ota_one_impl, ambit_ota_one, ambit_do_ota,
 *       ambit_do_flash, ambit_ota_run.
 * Fakes sit only at the seams: UART ping / version read / OTA stream, the ROM
 * loader (connect, regions, reset), SD, NVS, the download and the quiesce.
 * Protection switches on INSIDE a chosen blocking step (spread, ack flush,
 * quiesce, suspend, download, fetch, ping, version read, ROM probe, preflight,
 * bus wait, ROM connect) or while a channel is being written (that write must
 * finish). No erase or OTA_BEGIN may start after it.
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
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_NOT_ALLOWED 0x10D
#define TAG "ambit_host"
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
#define AMBIT_OTA_REBOOT_WAIT_MS 5000
/* ambit_flash.c */
#define AMBIT_FW_ROOT "/sdcard/ambit_fw"
#define FLASH_UART 0
#define ROM_BAUD 115200
#define BUS_WAIT_MS 3000
#define FLASH_BAUD_DEF 460800
#define BOOT_NVS_NS "ambit_fl"
#define BOOT_FAIL_MAX 3
#define BOOT_VERIFY_DELAY_MS 4000
#define BOOT_VERIFY_TRIES 2

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
typedef struct { bool ok; int chip; int regions_written; uint32_t total_bytes; } ambit_flash_image_result_t;
typedef struct { uint8_t major, minor, batch; char dir[64]; } ambit_flash_target_t;
typedef struct { uint8_t major, minor, batch; } ambit_fw_info_t;
typedef struct { bool valid; } ambit_device_info_t;
typedef enum { PP_STATE_NORMAL = 0, PP_STATE_NO_FLASH = 1, PP_STATE_HOLD = 2 } pp_state_t;
typedef enum { POWER_PROTECT_SKIP_FLASH_RUN = 0, POWER_PROTECT_SKIP_AMBIT_RUN, POWER_PROTECT_SKIP_OTA } power_protect_skip_t;

/* ── scenario state and what the code did ─────────────────────────────── */
static const char *sc;
static bool pp_on;
static unsigned skips, suspends, resumes, comms_down, comms_up, maint_depth, latches, fail_notes, delays;
static unsigned downloads, fetches, resets, sessions, writes[UART_SENSOR_NUM_CHANNELS];
static int cur_ch = -1;
static bool is(const char *s) { return strcmp(sc, s) == 0; }
static void flip_if(const char *s) { if (is(s)) pp_on = true; }

bool power_protect_blocks_flash(void) { return pp_on; }
pp_state_t power_protect_state(void) { return pp_on ? PP_STATE_NO_FLASH : PP_STATE_NORMAL; }
const char *pp_state_name(pp_state_t s) { return s == PP_STATE_NO_FLASH ? "no_flash" : "normal"; }
void power_protect_note_skip(power_protect_skip_t w) { assert(w == POWER_PROTECT_SKIP_OTA); ++skips; }

static void vTaskDelay(uint32_t t) { (void)t; if (++delays == 1) flip_if("flash_flip_during_ack"); }
static void workload_suspend(void) { ++suspends; flip_if("flash_flip_during_suspend"); }
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
    printf("REPORT %s %s ch=%u id=%s detail=%s\n", type, state, (unsigned)ch, id ? id : "-", detail ? detail : "-");
}
static void report(const char *state, uint8_t ch, const char *id) { report_as("ambit_ota_status", state, ch, id, NULL); }
static void report_detail(const char *state, uint8_t ch, const char *id, const char *d) { report_as("ambit_ota_status", state, ch, id, d); }
static void report_busy(const ambit_ota_req_t *r) { (void)r; assert(0); }
static void latch_set(const char *key, const char *id) { (void)key; (void)id; ++latches; }
static bool already_applied(const char *key, const char *id) { (void)key; (void)id; return false; }
static void flash_fail_note(const char *id) { (void)id; ++fail_notes; }
static void flash_fail_clear(void) {}
static void wait_for_fleet_slot(void) { flip_if("ota_flip_during_spread"); }
/* The real one suspends the workload and waits 65 s. */
static void ambit_quiesce_for_idle(void) { workload_suspend(); flip_if("ota_flip_during_quiesce"); }
static bool sdcard_is_mounted(void) { return true; }
static esp_err_t sdcard_mount(void) { return ESP_OK; }
static bool sdcard_io_begin(void) { return true; }
static void sdcard_io_end(void) {}
static esp_err_t ambit_stage_remove(const ambit_stage_t *s) { (void)s; return ESP_OK; }
static esp_err_t http_get_to_stage(const char *url, ambit_stage_t *st)
{
    (void)url; ++downloads;
    snprintf(st->path, sizeof st->path, "/sdcard/stage.bin"); st->len = 4096;
    flip_if("ota_flip_during_download");
    return ESP_OK;
}
static cmd_result_t cmd_uart_ping(uint8_t ch, bool *connected)
{
    *connected = true;
    if (ch == 0) flip_if("ota_flip_during_ping");
    return (cmd_result_t){ESP_OK, ""};
}
/* The pre-stream identity read (UART, blocking). */
static bool ambit_log_version(uint8_t ch, const char *when)
{
    if (ch == 0 && strcmp(when, "before") == 0) flip_if("ota_flip_during_identity");
    return true;
}
/* OTA_BEGIN + data + end: the destructive part. */
static bool ambit_stream_image(uint8_t ch, const ambit_stage_t *st)
{
    (void)st; ++writes[ch];
    if (ch == 1) flip_if("ota_flip_mid_sweep");   /* begins while ch1 streams: it finishes */
    return true;
}
static cmd_result_t cmd_ambit_ota_confirm(uint8_t ch, uint8_t *st) { (void)ch; *st = 0; return (cmd_result_t){ESP_OK, ""}; }
static void cmd_ambit_device_info_invalidate(uint8_t ch) { (void)ch; }
static cmd_result_t cmd_ambit_device_info(uint8_t ch, ambit_device_info_t *i) { (void)ch; i->valid = true; return (cmd_result_t){ESP_OK, ""}; }
static esp_err_t ambit_fw_fetch(const char *root, const char *url, uint8_t a, uint8_t b, uint8_t c,
                                const ambit_fw_http_t *http, ambit_fw_fetch_result_t *out)
{
    (void)root; (void)url; (void)a; (void)b; (void)c; (void)http; ++fetches;
    *out = (ambit_fw_fetch_result_t){ "ok", 4, 0, 0, 1000 };
    flip_if("flash_flip_during_fetch");
    return ESP_OK;
}
static esp_err_t ambit_flash_probe(uint8_t ch, ambit_flash_probe_result_t *out)
{
    out->connected = true;
    if (ch == 0) flip_if("flash_flip_during_probe");   /* resets into ROM, waits, connects */
    return ESP_OK;
}
static void ambit_do_versions(const char *id) { (void)id; assert(0); }
static void ambit_do_probe(const ambit_ota_req_t *r) { (void)r; assert(0); }
static void ambit_do_attach(void) { assert(0); }
static void ambit_attach_gate_checker_done(void) {}

/* ── ambit_flash.c seams: ROM loader, SD preflight, UART bus, NVS ─────── */
typedef int esp_loader_error_t;
#define ESP_LOADER_SUCCESS 0
#define ESP_LOADER_ERROR_INVALID_TARGET 9
typedef enum { ESP_UNKNOWN_CHIP = -1, ESP32C3_CHIP = 3 } target_chip_t;
typedef struct { int unused; } esp_loader_t;
typedef struct { int unused; } esp_loader_connect_args_t;
#define ESP_LOADER_CONNECT_DEFAULT() { 0 }
typedef struct { int unused; } esp_loader_port_ops_t;
typedef struct { const esp_loader_port_ops_t *ops; } esp_loader_port_t;
typedef struct { esp_loader_port_t base; uint8_t channel; int64_t time_end_us; } ambit_flash_port_t;
static const esp_loader_port_ops_t ambit_flash_port_ops;
static esp_err_t ambit_flash_preflight(const char *root, const char *dir, const char **f, size_t n)
{
    (void)root; (void)dir; (void)f; (void)n;
    flip_if("flash_flip_during_preflight");   /* SHA-256 of every region file */
    return ESP_OK;
}
static esp_err_t uart_sensors_flash_session_begin(uint8_t ch, uint32_t ms)
{
    (void)ms; assert(cur_ch == -1); cur_ch = ch; ++sessions;
    flip_if("flash_flip_during_bus_wait");
    return ESP_OK;
}
static void uart_sensors_flash_session_end(uint8_t ch) { assert(cur_ch == ch); cur_ch = -1; }
static esp_loader_error_t esp_loader_init_serial(esp_loader_t *l, esp_loader_port_t *p) { (void)l; (void)p; return ESP_LOADER_SUCCESS; }
static esp_loader_error_t esp_loader_connect_with_stub(esp_loader_t *l, esp_loader_connect_args_t *a)
{
    (void)l; (void)a;
    flip_if("flash_flip_during_rom_connect");   /* ROM reset + sync + stub upload */
    flip_if("attach_flip_during_rom_connect");
    return ESP_LOADER_SUCCESS;
}
static target_chip_t esp_loader_get_target(esp_loader_t *l) { (void)l; return ESP32C3_CHIP; }
static esp_loader_error_t esp_loader_change_transmission_rate(esp_loader_t *l, uint32_t b) { (void)l; (void)b; return ESP_LOADER_SUCCESS; }
static void esp_loader_reset_target(esp_loader_t *l) { (void)l; ++resets; }
static void uart_set_baudrate(int uart, uint32_t b) { (void)uart; (void)b; }
static bool boot_read_version(uint8_t ch, ambit_fw_info_t *fw) { (void)ch; *fw = (ambit_fw_info_t){1, 4, 0}; return true; }
/* One NVS slot per channel ("bf<ch>" = "<ver>:<n>"). */
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
static char nvs_bf[UART_SENSOR_NUM_CHANNELS][24];
static esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)m; assert(!strcmp(ns, BOOT_NVS_NS)); *h = 1; return ESP_OK; }
static esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len)
{
    (void)h;
    const int ch = k[2] - '0';
    if (!nvs_bf[ch][0]) return 0x1102;
    snprintf(out, *len, "%s", nvs_bf[ch]);
    return ESP_OK;
}
static esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h; snprintf(nvs_bf[k[2] - '0'], 24, "%s", v); return ESP_OK; }
static esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; nvs_bf[k[2] - '0'][0] = '\0'; return ESP_OK; }
static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static void nvs_close(nvs_handle_t h) { (void)h; }

#define s_cfg ambit_cfg
#include "ambit-worker-production.inc"
#undef s_cfg

/* The region writer (erase + write + MD5) is the destructive step. */
static esp_loader_error_t flash_one_region(esp_loader_t *l, const char *dir, const flash_region_t *r, uint32_t *nb)
{
    (void)l; (void)dir; *nb = 1000;
    assert(cur_ch >= 0);
    if (r == &s_regions[0]) {
        ++writes[cur_ch];
        if (cur_ch == 0) flip_if("flash_flip_mid_sweep");   /* begins during ch0's flash: it finishes */
    }
    return ESP_LOADER_SUCCESS;
}

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
    assert(cur_ch == -1);                                     /* every UART session ended */
}

static unsigned total_writes(void) { return writes[0] + writes[1] + writes[2] + writes[3]; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    sc = argv[1];
    /* ── AMBIT OTA ── */
    if (is("ota_protected_at_entry")) {
        pp_on = true;
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, true, "");
        assert(downloads == 0 && total_writes() == 0 && suspends == 0 && comms_down == 0 && skips == 1);
    } else if (is("ota_flip_during_spread")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, true, "");
        assert(downloads == 0 && total_writes() == 0 && suspends == 0 && comms_down == 0 && skips == 1);
    } else if (is("ota_flip_during_quiesce")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, false, "");
        assert(downloads == 0 && total_writes() == 0 && suspends == 1 && comms_down == 1 && skips == 1);
    } else if (is("ota_flip_during_download")) {
        run(AMBIT_OP_OTA, 2, false, "");
        assert(downloads == 1 && total_writes() == 0 && latches == 0 && skips == 1);
    } else if (is("ota_flip_during_ping") || is("ota_flip_during_identity")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, false, "");
        assert(downloads == 1 && total_writes() == 0 && latches == 0 && skips == 1);
    } else if (is("ota_flip_mid_sweep")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, false, "");
        /* ch0 and ch1 (started before protection) finished; ch2, ch3 never started. */
        assert(writes[0] == 1 && writes[1] == 1 && writes[2] == 0 && writes[3] == 0);
        assert(latches == 0 && skips == 1);
    /* ── ambit_flash ── */
    } else if (is("flash_flip_during_ack") || is("flash_flip_during_suspend")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "https://example.test/manifest.json");
        assert(fetches == 0 && total_writes() == 0 && sessions == 0 && skips == 1 && fail_notes == 0);
        assert(suspends == (is("flash_flip_during_ack") ? 0u : 1u));
    } else if (is("flash_flip_during_fetch")) {
        run(AMBIT_OP_FLASH, 1, false, "https://example.test/manifest.json");
        assert(fetches == 1 && total_writes() == 0 && sessions == 0 && latches == 0 && fail_notes == 0 && skips == 1);
    } else if (is("flash_flip_during_probe") || is("flash_flip_during_preflight") ||
               is("flash_flip_during_bus_wait") || is("flash_flip_during_rom_connect")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "");
        /* ch0 went through its blocking setup but was never erased, and was
         * reset back into its application; ch1..3 never started. */
        assert(total_writes() == 0 && sessions == 1 && resets == 1);
        assert(latches == 0 && fail_notes == 0 && skips == 1);
    } else if (is("flash_flip_mid_sweep")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "");
        assert(writes[0] == 1 && writes[1] == 0 && writes[2] == 0 && writes[3] == 0 && resets == 1);
        assert(latches == 0 && fail_notes == 0 && skips == 1);
    /* ── attach update: a power refusal does not consume the fail cap ── */
    } else if (is("attach_flip_during_rom_connect")) {
        const ambit_flash_target_t tgt = { 1, 4, 0, "/sdcard/ambit_fw/1.4.0" };
        snprintf(nvs_bf[2], sizeof nvs_bf[2], "1.4.0:1");   /* one earlier failed attempt */
        assert(ambit_flash_attach_flash(2, &tgt) == ESP_ERR_NOT_ALLOWED);
        assert(writes[2] == 0 && resets == 1 && cur_ch == -1);
        assert(strcmp(nvs_bf[2], "1.4.0:1") == 0);          /* the bump was undone: 1, not 2 */
        nvs_bf[2][0] = '\0';                                /* first attempt ever, protected */
        assert(ambit_flash_attach_flash(2, &tgt) == ESP_ERR_NOT_ALLOWED && nvs_bf[2][0] == '\0');
    } else if (is("attach_unprotected")) {
        const ambit_flash_target_t tgt = { 1, 4, 0, "/sdcard/ambit_fw/1.4.0" };
        snprintf(nvs_bf[2], sizeof nvs_bf[2], "1.4.0:1");
        assert(ambit_flash_attach_flash(2, &tgt) == ESP_OK && writes[2] == 1 && nvs_bf[2][0] == '\0');
    /* ── controls ── */
    } else if (is("ota_unprotected")) {
        run(AMBIT_OP_OTA, AMBIT_OTA_CH_ALL, true, "");
        assert(total_writes() == 4 && latches == 1 && skips == 0);
    } else if (is("flash_unprotected")) {
        run(AMBIT_OP_FLASH, AMBIT_OTA_CH_ALL, false, "");
        assert(total_writes() == 4 && latches == 1 && skips == 0 && resets == 4);
    } else {
        fprintf(stderr, "unknown scenario %s\n", sc);
        return 2;
    }
    printf("PASS %s\n", sc);
    return 0;
}

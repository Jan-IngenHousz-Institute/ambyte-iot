#include "ambit_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambit_flash.h"
#include "ambit_protocol.h"
#include "device_commands.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sd_card.h"
#include "uart_sensor_port.h"

#define TAG "ambit_ota"

/* 10 KiB: the OTA path fit in 8 KiB, but the FLASH op adds esp-serial-flasher's
 * connect/MD5 machinery on this same stack. The task is spawned on demand and
 * exits when idle, so the extra 2 KiB is transient, not resident. */
#define AMBIT_OTA_TASK_STACK   10240
#define AMBIT_OTA_TASK_PRIO    4          /* below lua_runner(10); not latency-critical */
#define AMBIT_OTA_URL_MAX      256
#define AMBIT_OTA_MAX_RETRY    4          /* per-chunk resend attempts on CRC/transport error */
#define AMBIT_OTA_REBOOT_WAIT_MS 5000     /* let the C3 reboot into the new image before re-querying */
#define AMBIT_OTA_DL_BUF       4096
#define AMBIT_OTA_ID_MAX       64
#define AMBIT_FW_PATH          "/sdcard/ambit_fw.bin"

#define NVS_NS                 "ambit_ota"
#define KEY_APPLIED            "applied_id"   /* id of the last *successfully* applied OTA */
#define KEY_APPLIED_FLASH      "applied_fl"   /* separate latch for ROM-flash ids: a flash
                                               * success must not clobber the OTA latch (or
                                               * vice versa) and un-dedupe the other op's
                                               * retained trigger */
#define KEY_FLASH_FAIL_ID      "fl_fail_id"   /* last FAILED flash id + attempt count: a */
#define KEY_FLASH_FAIL_N       "fl_fail_n"    /* retained trigger with a persistent failure
                                               * (missing SD folder, no ROM answer) must not
                                               * bounce Lua+MQTT and hard-reset the AMBITs
                                               * forever — cap the retries per id */
#define AMBIT_FLASH_FAIL_MAX   3

#define AMBIT_OP_OTA       0
#define AMBIT_OP_VERSIONS  1
#define AMBIT_OP_PROBE     2          /* ROM-bootloader probe (chip + MAC report) */
#define AMBIT_OP_FLASH     3          /* full 4-region ROM flash from an SD folder */

typedef struct {
    uint8_t op;                       /* AMBIT_OP_* */
    uint8_t channel;
    char    url[AMBIT_OTA_URL_MAX];   /* OTA: download URL; FLASH: /sdcard/ambit_fw/<ver> dir */
    char    id[AMBIT_OTA_ID_MAX];
} ambit_ota_req_t;

static ambit_ota_config_t s_cfg;
static bool               s_ready;    /* init done; dispatch via the shared worker (s_cfg.submit) */

/* ── dedupe latch (NVS) ──────────────────────────────────────────────────
 * Mirror of ota_update: an id is latched only on a *successful* update, so a
 * retained/duplicate MQTT trigger for an already-applied id is ignored, while a
 * failed attempt stays retryable under the same id. CLI passes a NULL id (an
 * operator-initiated update is never deduped). */
static void latch_set(const char *key, const char *id)
{
    if (id == NULL || id[0] == '\0') return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, key, id) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static bool already_applied(const char *key, const char *id)
{
    if (id == NULL || id[0] == '\0') return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char prev[AMBIT_OTA_ID_MAX] = "";
    size_t len = sizeof prev;
    esp_err_t err = nvs_get_str(h, key, prev, &len);
    nvs_close(h);
    return err == ESP_OK && strcmp(prev, id) == 0;
}

/* ── flash retry cap ──────────────────────────────────────────────────────
 * A FAILED flash id is remembered with an attempt count. Success-only latching
 * keeps transient failures retryable, but a flash whose failure is persistent
 * (typo'd version, unstaged SD, zero ROM-answering channels) would otherwise
 * loop forever off a retained trigger — each cycle bouncing Lua + MQTT and
 * hard-resetting every AMBIT. After AMBIT_FLASH_FAIL_MAX attempts the id is
 * refused; the operator retries under a fresh id once the cause is fixed. */
static uint8_t flash_fail_count(const char *id)
{
    if (id == NULL || id[0] == '\0') return 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    char prev[AMBIT_OTA_ID_MAX] = "";
    size_t len = sizeof prev;
    uint8_t n = 0;
    if (nvs_get_str(h, KEY_FLASH_FAIL_ID, prev, &len) != ESP_OK ||
        strcmp(prev, id) != 0 ||
        nvs_get_u8(h, KEY_FLASH_FAIL_N, &n) != ESP_OK) {
        n = 0;
    }
    nvs_close(h);
    return n;
}

static void flash_fail_note(const char *id)
{
    if (id == NULL || id[0] == '\0') return;   /* CLI (no id): never capped */
    uint8_t n = flash_fail_count(id);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, KEY_FLASH_FAIL_ID, id) == ESP_OK &&
        nvs_set_u8(h, KEY_FLASH_FAIL_N, (uint8_t)(n + 1)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void flash_fail_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, KEY_FLASH_FAIL_ID);
    nvs_erase_key(h, KEY_FLASH_FAIL_N);
    nvs_commit(h);
    nvs_close(h);
}

/* ── best-effort status report (console always; MQTT if connected) ───────── */

/* Minimal JSON string escape for the attacker-influenced id in status reports. */
static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    if (in == NULL) { if (cap) out[0] = '\0'; return; }
    for (const char *p = in; *p != '\0' && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c >= 0x20)        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

/* `detail` (nullable) adds a "detail":"…" reason field — previously a failed
 * report carried only the state, giving a remote operator no clue why. */
static void report_as(const char *type, const char *state, uint8_t ch, const char *id,
                      const char *detail)
{
    if (s_cfg.publish == NULL || s_cfg.status_topic == NULL || s_cfg.status_topic[0] == '\0') {
        return;
    }
    if (s_cfg.is_connected != NULL && !s_cfg.is_connected()) {
        return;   /* MQTT not back yet after resume — console log already covered it */
    }
    char esc_id[AMBIT_OTA_ID_MAX * 2 + 1] = "";
    json_escape(esc_id, sizeof esc_id, id);
    char esc_detail[160] = "";
    if (detail != NULL) json_escape(esc_detail, sizeof esc_detail, detail);
    char msg[416];
    int n = snprintf(msg, sizeof msg,
        "{\"type\":\"%s\",\"device_id\":\"%s\",\"id\":\"%s\",\"channel\":%u,\"state\":\"%s\"%s%s%s}",
        type, s_cfg.device_id ? s_cfg.device_id : "", esc_id, (unsigned)ch, state,
        detail ? ",\"detail\":\"" : "", esc_detail, detail ? "\"" : "");
    if (n > 0 && (size_t)n < sizeof msg) {
        int msg_id = 0;
        s_cfg.publish(s_cfg.status_topic, msg, (size_t)n, &msg_id);
    }
}

static void report(const char *state, uint8_t ch, const char *id)
{
    report_as("ambit_ota_status", state, ch, id, NULL);
}

static void report_detail(const char *state, uint8_t ch, const char *id, const char *detail)
{
    report_as("ambit_ota_status", state, ch, id, detail);
}

/* Reject an op because the global maintenance lock is held by another update
 * type — reported under the op's own status type so the operator sees why. */
static void report_busy(const ambit_ota_req_t *r)
{
    const char *detail = "another maintenance op is in progress";
    switch (r->op) {
    case AMBIT_OP_FLASH:    report_as("ambit_flash_status", "busy", r->channel, r->id, detail); break;
    case AMBIT_OP_PROBE:    report_as("ambit_probe",        "busy", r->channel, r->id, detail); break;
    case AMBIT_OP_VERSIONS: report_as("ambit_versions",     "busy", r->channel, r->id, detail); break;
    default:                report_detail("busy", r->channel, r->id, detail);                   break;
    }
}

/* ── HTTP GET → file on SD ────────────────────────────────────────────────
 * Streaming download (the image is too big to buffer in RAM). Requires a
 * direct-200 URL (raw.githubusercontent.com/… or a pre-resolved link); a 3xx
 * redirect is reported as an error rather than silently saving an HTML page
 * (release-asset 302 following is a follow-up). Same proven TLS/buffer settings
 * as the ambyte self-OTA (cert bundle validates GitHub + its CDN; 4 KiB buffers
 * fit GitHub's long signed-redirect URLs). */
static esp_err_t http_get_to_file_impl(const char *url, const char *path, size_t *out_size);

/* SD RW-gate wrapper (audit R-6): guard the download's fwrite loop against a monitor
 * teardown freeing the volume mid-write. */
static esp_err_t http_get_to_file(const char *url, const char *path, size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!sdcard_io_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t rc = http_get_to_file_impl(url, path, out_size);
    sdcard_io_end();
    return rc;
}

static esp_err_t http_get_to_file_impl(const char *url, const char *path, size_t *out_size)
{
    *out_size = 0;

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }

    int64_t clen   = esp_http_client_fetch_headers(c);
    int     status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d (expected 200)%s", status,
                 (status >= 300 && status < 400)
                     ? " — redirect; use a direct raw.githubusercontent.com URL" : "");
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for write", path);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(AMBIT_OTA_DL_BUF);
    if (buf == NULL) {
        fclose(f);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        int r = esp_http_client_read(c, (char *)buf, AMBIT_OTA_DL_BUF);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;   /* EOF (content-length reached or stream closed) */
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { err = ESP_ERR_NO_MEM; break; }
        total += (size_t)r;
        vTaskDelay(1);       /* yield so the idle task is fed on a fast link */
    }

    free(buf);
    fclose(f);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && clen > 0 && total != (size_t)clen) {
        ESP_LOGE(TAG, "short download: %u of %lld bytes", (unsigned)total, (long long)clen);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) *out_size = total;
    return err;
}

/* ── version read (cmd 33/2) — pre/post-OTA confirmation ─────────────────── */

/* Log the AMBIT's reported FW version. Returns true if it answered (image alive). */
static bool ambit_log_version(uint8_t ch, const char *when)
{
    uint8_t buf[64];
    size_t  len = 0;
    cmd_result_t r = cmd_ambit_get_info(ch, AMBIT_INFO_FW, buf, sizeof buf, &len);
    if (r.status == ESP_OK && len >= sizeof(ambit_fw_info_t)) {
        const ambit_fw_info_t *fw = (const ambit_fw_info_t *)buf;
        ESP_LOGW(TAG, "AMBIT%u fw %s: v%u.%u.%u (size=%lu)", ch + 1, when,
                 fw->major, fw->minor, fw->batch, (unsigned long)fw->size);
        return true;
    }
    ESP_LOGW(TAG, "AMBIT%u fw %s: no answer (%s)", ch + 1, when, esp_err_to_name(r.status));
    return false;
}

/* ── stream a staged image to one AMBIT ──────────────────────────────────── */

static bool ambit_stream_image(uint8_t ch, FILE *f, size_t img_size)
{
    uint8_t status = 0xFF;

    cmd_result_t r = cmd_ambit_ota_begin(ch, (uint32_t)img_size, &status);
    if (r.status != ESP_OK || status != 0) {
        ESP_LOGE(TAG, "OTA_BEGIN failed (%s, status=%u)", esp_err_to_name(r.status), status);
        return false;
    }

    uint8_t  buf[AMBIT_OTA_CHUNK_MAX];
    uint16_t seq = 0;
    size_t   sent = 0;
    int      last_decile = -1;
    size_t   n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        bool chunk_ok = false;
        for (int tries = 0; tries < AMBIT_OTA_MAX_RETRY && !chunk_ok; tries++) {
            r = cmd_ambit_ota_data(ch, seq, buf, (uint8_t)n, &status);
            if (r.status == ESP_OK && status == 0) {
                chunk_ok = true;
            } else {
                ESP_LOGW(TAG, "chunk seq=%u try=%d: err=%s status=%u",
                         seq, tries + 1, esp_err_to_name(r.status), status);
            }
        }
        if (!chunk_ok) {
            ESP_LOGE(TAG, "chunk seq=%u failed after %d tries (status=%u) — aborting",
                     seq, AMBIT_OTA_MAX_RETRY, status);
            cmd_ambit_ota_abort(ch, &status);
            return false;
        }
        sent += n;
        seq++;
        int decile = (img_size > 0) ? (int)(sent * 10 / img_size) : 0;
        if (decile != last_decile) {
            ESP_LOGW(TAG, "  streaming %d%% (%u/%u B, %u chunks)",
                     decile * 10, (unsigned)sent, (unsigned)img_size, (unsigned)seq);
            last_decile = decile;
        }
    }

    r = cmd_ambit_ota_end(ch, &status);
    if (r.status != ESP_OK || status != 0) {
        ESP_LOGE(TAG, "OTA_END failed (%s, status=%u) — AMBIT kept its old image",
                 esp_err_to_name(r.status), status);
        return false;
    }
    return true;
}

/* ── one channel: stream the staged image + confirm after reboot ──────────── */

/* Stream AMBIT_FW_PATH (img_size bytes) to channel `ch`, then — after the C3
 * reboots into the new (PENDING_VERIFY) image — confirm it ONLY if it answers.
 * If it doesn't answer or the confirm fails, the C3 bootloader rolls back to the
 * previous image on its next reboot. Returns true only when confirmed healthy. */
static bool ambit_ota_one_impl(uint8_t ch, size_t img_size);

/* SD RW-gate wrapper (audit R-6): guard the fopen + fread stream of the AMBIT image
 * from SD against a monitor teardown freeing the volume mid-stream. */
static bool ambit_ota_one(uint8_t ch, size_t img_size)
{
    if (!sdcard_io_begin()) return false;
    bool ok = ambit_ota_one_impl(ch, img_size);
    sdcard_io_end();
    return ok;
}

static bool ambit_ota_one_impl(uint8_t ch, size_t img_size)
{
    ambit_log_version(ch, "before");

    FILE *f = fopen(AMBIT_FW_PATH, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot reopen %s for read", AMBIT_FW_PATH);
        return false;
    }
    bool streamed = ambit_stream_image(ch, f, img_size);
    fclose(f);
    if (!streamed) return false;

    ESP_LOGW(TAG, "OTA_END ok — AMBIT%u rebooting; waiting %d ms then re-checking",
             ch + 1, AMBIT_OTA_REBOOT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(AMBIT_OTA_REBOOT_WAIT_MS));

    /* Retry the post-reboot read: a slow boot or one dropped reply must NOT be
     * mistaken for a dead image (that would skip confirm and force a needless
     * rollback on the C3's next reboot). */
    bool alive = false;
    for (int tries = 0; tries < 3 && !alive; tries++) {
        if (tries) vTaskDelay(pdMS_TO_TICKS(1000));
        alive = ambit_log_version(ch, "after ");
    }
    if (!alive) {
        ESP_LOGE(TAG, "AMBIT%u not answering after OTA — NOT confirming; it will roll "
                      "back to the previous image on its next reboot", ch + 1);
        return false;
    }
    uint8_t st = 0xFF;
    for (int tries = 0; tries < 3; tries++) {
        cmd_result_t cr = cmd_ambit_ota_confirm(ch, &st);
        if (cr.status == ESP_OK && st == 0) {
            ESP_LOGW(TAG, "AMBIT%u image confirmed — rollback cancelled", ch + 1);
            return true;
        }
        ESP_LOGW(TAG, "OTA_CONFIRM try %d: %s st=%u", tries + 1, esp_err_to_name(cr.status), st);
    }
    ESP_LOGE(TAG, "AMBIT%u confirm failed — image will roll back on its next reboot", ch + 1);
    return false;
}

/* ── one OTA request (single channel, or a sweep of all present channels) ──── */

static void ambit_do_ota(const ambit_ota_req_t *r)
{
    const char *url = r->url;
    const bool  all = (r->channel == AMBIT_OTA_CH_ALL);
    if (all) {
        ESP_LOGW(TAG, "AMBIT OTA requested: ch=all id=%s url=%s", r->id[0] ? r->id : "(none)", url);
    } else {
        ESP_LOGW(TAG, "AMBIT OTA requested: ch=%u id=%s url=%s",
                 r->channel, r->id[0] ? r->id : "(none)", url);
    }

    if (strstr(url, "/blob/") != NULL || strstr(url, "/tree/") != NULL) {
        ESP_LOGE(TAG, "URL is a GitHub web page (/blob/ or /tree/), not a file:");
        ESP_LOGE(TAG, "  %s", url);
        ESP_LOGE(TAG, "use a direct .bin — raw.githubusercontent.com/<owner>/<repo>/<branch>/<path>");
        report_detail("failed", r->channel, r->id,
                      "bad_url: use a direct .bin, not a /blob//tree/ web url");
        return;
    }

    /* On-receipt ack — MUST publish BEFORE comms_suspend() (after it MQTT is gone).
     * The terminal report only comes minutes later after the download + stream +
     * reconnect, so without this the operator (and the fleet-OTA notebook) sees
     * nothing on receipt. */
    report("accepted", r->channel, r->id);
    vTaskDelay(pdMS_TO_TICKS(500));   /* flush the ack before MQTT drops */

    /* Quiesce: free the UART (stop Lua) and the heap/TLS (stop MQTT). */
    if (s_cfg.workload_suspend != NULL) s_cfg.workload_suspend();
    if (s_cfg.comms_suspend != NULL) s_cfg.comms_suspend();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Per-channel outcome for the "all" sweep, reported AFTER comms resume: the
     * sweep runs inside the MQTT blackout, so reporting each result inline (as this
     * used to) published into a dead session and dropped it. Mirrors the flash
     * path's deferred per-channel reporting. */
    enum { OTA_SKIP = -1, OTA_FAIL = 0, OTA_OK = 1, OTA_ABSENT = 2 };
    int8_t res[UART_SENSOR_NUM_CHANNELS];
    memset(res, OTA_SKIP, sizeof res);   /* -1 = 0xFF per byte */

    bool ok = false;
    const char *detail = NULL;
    if (!sdcard_is_mounted() && sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD not available — cannot stage the AMBIT image");
        detail = "SD not available";
    } else {
        size_t    img_size = 0;
        esp_err_t err = http_get_to_file(url, AMBIT_FW_PATH, &img_size);   /* download once */
        if (err != ESP_OK || img_size == 0) {
            ESP_LOGE(TAG, "download failed (%s, %u bytes)", esp_err_to_name(err), (unsigned)img_size);
            detail = "download failed";
        } else {
            ESP_LOGW(TAG, "downloaded %u bytes -> %s", (unsigned)img_size, AMBIT_FW_PATH);
            if (all) {
                /* Sweep every present channel, sequentially (the UART is shared). */
                int present = 0, ok_count = 0;
                for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++) {
                    bool connected = false;
                    cmd_result_t pr = cmd_uart_ping(c, &connected);
                    if (pr.status != ESP_OK || !connected) {
                        ESP_LOGW(TAG, "AMBIT%u: not present — skipping", c + 1);
                        res[c] = OTA_ABSENT;
                        continue;
                    }
                    present++;
                    bool ok_c = ambit_ota_one(c, img_size);
                    res[c] = ok_c ? OTA_OK : OTA_FAIL;
                    if (ok_c) ok_count++;
                }
                ESP_LOGW(TAG, "AMBIT OTA all: %d/%d present channels updated", ok_count, present);
                ok = (present > 0 && ok_count == present);
                if (!ok) detail = (present == 0) ? "no AMBIT present" : "one or more channels failed";
            } else {
                ok = ambit_ota_one(r->channel, img_size);
                if (!ok) detail = "stream/confirm failed";
            }
        }
    }

    /* Latch BEFORE comms resume (like the flash path): a reconnect's resubscribe
     * redelivers a retained trigger immediately, and it must hit an already-written
     * latch or the whole sweep runs twice. Only on full success. */
    if (ok) latch_set(KEY_APPLIED, r->id);

    if (s_cfg.comms_resume != NULL) s_cfg.comms_resume();
    if (s_cfg.workload_resume != NULL) s_cfg.workload_resume();

    /* Give MQTT a bounded window to reconnect so the reports actually land
     * (report_* drop while disconnected; the console log has them regardless).
     * Without this the terminal report fired the instant after comms_resume, ahead
     * of the TLS reconnect, and was almost always dropped. Mirrors the flash path. */
    if (s_cfg.is_connected != NULL) {
        for (int i = 0; i < 60 && !s_cfg.is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!s_cfg.is_connected()) {
            ESP_LOGW(TAG, "MQTT not back after 30 s — OTA reports dropped "
                          "(verify by effect: ambit_versions)");
        }
    }

    /* Deferred per-channel results (populated only for the "all" sweep). */
    for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++) {
        if (res[c] == OTA_SKIP) continue;
        report(res[c] == OTA_OK ? "success" : (res[c] == OTA_ABSENT ? "absent" : "failed"),
               c, r->id);
    }

    if (ok) {
        ESP_LOGW(TAG, "AMBIT OTA SUCCESS (id=%s)", r->id[0] ? r->id : "(none)");
        report("success", r->channel, r->id);
    } else {
        ESP_LOGE(TAG, "AMBIT OTA FAILED (id=%s)", r->id[0] ? r->id : "(none)");
        report_detail("failed", r->channel, r->id, detail);
    }
}

/* ── fleet version report ─────────────────────────────────────────────────
 * Sweep all channels (cmd 33/2), log a per-channel line, and publish one JSON
 * report. Runs on the worker task (off the MQTT loop). No quiesce — the version
 * read is a quick UART transaction the uart_sensors mutex serializes with Lua. */
static void ambit_do_versions(const char *id)
{
    char esc_id[AMBIT_OTA_ID_MAX * 2 + 1] = "";
    json_escape(esc_id, sizeof esc_id, id);

    char buf[512];
    int  o = snprintf(buf, sizeof buf,
        "{\"type\":\"ambit_versions\",\"device_id\":\"%s\",\"id\":\"%s\",\"channels\":[",
        s_cfg.device_id ? s_cfg.device_id : "", esc_id);

    for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++) {
        if (o < 0 || o >= (int)sizeof buf - 48) break;   /* leave room for one entry + "]}" */
        const char *sep = (c == 0) ? "" : ",";
        bool connected = false;
        cmd_result_t pr = cmd_uart_ping(c, &connected);
        if (pr.status == ESP_OK && connected) {
            uint8_t vb[64];
            size_t  len = 0;
            cmd_result_t r = cmd_ambit_get_info(c, AMBIT_INFO_FW, vb, sizeof vb, &len);
            if (r.status == ESP_OK && len >= sizeof(ambit_fw_info_t)) {
                const ambit_fw_info_t *fw = (const ambit_fw_info_t *)vb;
                ESP_LOGW(TAG, "AMBIT%u: v%u.%u.%u", c + 1, fw->major, fw->minor, fw->batch);
                o += snprintf(buf + o, sizeof buf - o,
                    "%s{\"ch\":%u,\"present\":true,\"version\":\"%u.%u.%u\"}",
                    sep, c, fw->major, fw->minor, fw->batch);
            } else {
                ESP_LOGW(TAG, "AMBIT%u: present, no version", c + 1);
                o += snprintf(buf + o, sizeof buf - o, "%s{\"ch\":%u,\"present\":true}", sep, c);
            }
        } else {
            ESP_LOGW(TAG, "AMBIT%u: absent", c + 1);
            o += snprintf(buf + o, sizeof buf - o, "%s{\"ch\":%u,\"present\":false}", sep, c);
        }
    }

    if (o > 0 && o < (int)sizeof buf - 2) {
        o += snprintf(buf + o, sizeof buf - o, "]}");
        if (s_cfg.publish != NULL && s_cfg.status_topic != NULL && s_cfg.status_topic[0] != '\0' &&
            (s_cfg.is_connected == NULL || s_cfg.is_connected())) {
            int msg_id = 0;
            s_cfg.publish(s_cfg.status_topic, buf, (size_t)o, &msg_id);
        }
    }
}

/* ── ROM-bootloader probe sweep ───────────────────────────────────────────
 * Drive each requested channel's straps into download mode, read chip + MAC,
 * reset back to the app, and publish one ambit_probe JSON report. Works with no
 * cooperating app firmware — this is how a remote operator distinguishes
 * "bricked but flashable" from "hardware absent". Lua is stopped for the window
 * (the flasher needs the shared UART); MQTT stays up (op takes seconds and
 * needs no TLS heap), so the report publishes immediately. */
static void ambit_do_probe(const ambit_ota_req_t *r)
{
    const bool all  = (r->channel == AMBIT_OTA_CH_ALL);
    const uint8_t from = all ? 0 : r->channel;
    const uint8_t to   = all ? UART_SENSOR_NUM_CHANNELS : (uint8_t)(r->channel + 1);
    ESP_LOGW(TAG, "AMBIT probe requested: ch=%s id=%s",
             all ? "all" : "one", r->id[0] ? r->id : "(none)");

    if (s_cfg.workload_suspend != NULL) s_cfg.workload_suspend();

    char esc_id[AMBIT_OTA_ID_MAX * 2 + 1] = "";
    json_escape(esc_id, sizeof esc_id, r->id);
    char buf[512];
    int  o = snprintf(buf, sizeof buf,
        "{\"type\":\"ambit_probe\",\"device_id\":\"%s\",\"id\":\"%s\",\"channels\":[",
        s_cfg.device_id ? s_cfg.device_id : "", esc_id);

    for (uint8_t c = from; c < to; c++) {
        if (o < 0 || o >= (int)sizeof buf - 96) break;   /* room for one entry + "]}" */
        const char *sep = (c == from) ? "" : ",";
        ambit_flash_probe_result_t pr;
        esp_err_t err = ambit_flash_probe(c, &pr);
        if (err == ESP_OK && pr.connected) {
            ESP_LOGW(TAG, "AMBIT%u: ROM OK chip=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     c + 1, pr.chip, pr.mac[0], pr.mac[1], pr.mac[2],
                     pr.mac[3], pr.mac[4], pr.mac[5]);
            o += snprintf(buf + o, sizeof buf - o,
                "%s{\"ch\":%u,\"rom\":true,\"chip\":%d,"
                "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
                sep, c, pr.chip, pr.mac[0], pr.mac[1], pr.mac[2],
                pr.mac[3], pr.mac[4], pr.mac[5]);
        } else if (err == ESP_ERR_TIMEOUT) {
            /* Couldn't take the UART bus (Lua didn't stop in time) — the channel
             * state is UNKNOWN, not absent. Report it distinctly so a remote
             * operator doesn't conclude "hardware dead" from a busy bus. */
            ESP_LOGW(TAG, "AMBIT%u: bus busy — probe indeterminate", c + 1);
            o += snprintf(buf + o, sizeof buf - o,
                          "%s{\"ch\":%u,\"error\":\"bus_busy\"}", sep, c);
        } else {
            ESP_LOGW(TAG, "AMBIT%u: no ROM response (%s)", c + 1, esp_err_to_name(err));
            o += snprintf(buf + o, sizeof buf - o, "%s{\"ch\":%u,\"rom\":false}", sep, c);
        }
    }

    if (s_cfg.workload_resume != NULL) s_cfg.workload_resume();

    if (o > 0 && o < (int)sizeof buf - 2) {
        o += snprintf(buf + o, sizeof buf - o, "]}");
        if (s_cfg.publish != NULL && s_cfg.status_topic != NULL && s_cfg.status_topic[0] != '\0' &&
            (s_cfg.is_connected == NULL || s_cfg.is_connected())) {
            int msg_id = 0;
            s_cfg.publish(s_cfg.status_topic, buf, (size_t)o, &msg_id);
        }
    }
}

/* ── full ROM flash from an SD version folder ─────────────────────────────
 * Strategy A over MQTT: flash the 4 region images (NVS@0x9000 never written) on
 * one channel, or on every channel whose ROM answers a probe — deliberately NOT
 * ping-gated like the OTA sweep, because reviving units whose app firmware is
 * dead/ancient is this op's main job. Quiesces Lua + MQTT for the whole sweep
 * (the flasher owns the UART for ~10-60 s per channel and must not compete for
 * heap), then resumes and reports per channel. */
static void ambit_do_flash(const ambit_ota_req_t *r)
{
    const bool all = (r->channel == AMBIT_OTA_CH_ALL);
    const char *dir = r->url;

    /* Re-check the dedupe latch at EXECUTION time: a duplicate delivery can pass
     * the enqueue-time check while the first copy is still in flight (queue depth
     * is 2), and would otherwise run a full second sweep after the first latched. */
    if (already_applied(KEY_APPLIED_FLASH, r->id)) {
        ESP_LOGI(TAG, "ambit_flash id=%s already applied — skipping queued duplicate", r->id);
        return;
    }
    ESP_LOGW(TAG, "AMBIT flash requested: ch=%s dir=%s id=%s",
             all ? "all" : "one", dir, r->id[0] ? r->id : "(none)");

    /* Per-channel outcome, reported after comms resume. */
    enum { FL_SKIP = -1, FL_FAIL = 0, FL_OK = 1, FL_ABSENT = 2, FL_BUSY = 3 };
    int8_t res[UART_SENSOR_NUM_CHANNELS];
    memset(res, FL_SKIP, sizeof res);   /* -1 = 0xFF per byte */

    /* On-receipt ack — publish BEFORE comms_suspend() (after it MQTT is gone for
     * the whole ~10-60 s/channel sweep and only the terminal report comes back). */
    report_as("ambit_flash_status", "accepted", r->channel, r->id, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));   /* flush the ack before MQTT drops */

    if (s_cfg.workload_suspend != NULL) s_cfg.workload_suspend();
    if (s_cfg.comms_suspend != NULL) s_cfg.comms_suspend();
    vTaskDelay(pdMS_TO_TICKS(500));

    bool ok = false;
    if (!sdcard_is_mounted() && sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD not available — no region images to flash");
    } else if (all) {
        int attempted = 0, ok_count = 0;
        for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++) {
            ambit_flash_probe_result_t pr;
            esp_err_t perr = ambit_flash_probe(c, &pr);
            if (perr == ESP_ERR_TIMEOUT) {
                /* Bus still held (Lua didn't stop) — indeterminate, not absent. */
                ESP_LOGW(TAG, "AMBIT%u: bus busy — skipping (state unknown)", c + 1);
                res[c] = FL_BUSY;
                continue;
            }
            if (perr != ESP_OK || !pr.connected) {
                ESP_LOGW(TAG, "AMBIT%u: no ROM response — skipping", c + 1);
                res[c] = FL_ABSENT;
                continue;
            }
            attempted++;
            ambit_flash_image_result_t fr;
            esp_err_t ferr = ambit_flash_image(c, dir, 0, &fr);
            res[c] = (ferr == ESP_OK) ? FL_OK : FL_FAIL;
            if (ferr == ESP_OK) {
                ok_count++;
                ESP_LOGW(TAG, "AMBIT%u: FLASH OK (%d regions, %u B)",
                         c + 1, fr.regions_written, (unsigned)fr.total_bytes);
            } else {
                ESP_LOGE(TAG, "AMBIT%u: FLASH FAILED (%s) after %d region(s)",
                         c + 1, esp_err_to_name(ferr), fr.regions_written);
            }
        }
        ESP_LOGW(TAG, "AMBIT flash all: %d/%d ROM-answering channels flashed", ok_count, attempted);
        ok = (attempted > 0 && ok_count == attempted);
    } else {
        ambit_flash_image_result_t fr;
        esp_err_t ferr = ambit_flash_image(r->channel, dir, 0, &fr);
        res[r->channel] = (ferr == ESP_OK) ? FL_OK
                        : (ferr == ESP_ERR_TIMEOUT) ? FL_BUSY : FL_FAIL;
        ok = (ferr == ESP_OK);
        ESP_LOGW(TAG, "AMBIT%u: flash %s", r->channel + 1, ok ? "OK" : "FAILED");
    }

    /* Latch BEFORE comms resume: the reconnect's resubscribe redelivers a
     * retained trigger immediately, and it must hit an already-written latch or
     * the whole sweep runs twice. Failures bump the per-id retry cap the same
     * way, so a persistent failure can't loop the fleet reset forever. */
    if (ok) {
        latch_set(KEY_APPLIED_FLASH, r->id);
        flash_fail_clear();
    } else {
        flash_fail_note(r->id);
    }

    if (s_cfg.comms_resume != NULL) s_cfg.comms_resume();
    if (s_cfg.workload_resume != NULL) s_cfg.workload_resume();

    /* Give MQTT a bounded window to reconnect so the reports actually go out
     * (report_as drops them when disconnected; the console log has them anyway). */
    if (s_cfg.is_connected != NULL) {
        for (int i = 0; i < 60 && !s_cfg.is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!s_cfg.is_connected()) {
            ESP_LOGW(TAG, "MQTT not back after 30 s — flash reports dropped "
                          "(verify by effect: ambit_versions)");
        }
    }
    for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++) {
        if (res[c] == FL_SKIP) continue;
        report_as("ambit_flash_status",
                  (res[c] == FL_OK) ? "flash_ok"
                  : (res[c] == FL_ABSENT) ? "absent"
                  : (res[c] == FL_BUSY) ? "bus_busy" : "flash_failed",
                  c, r->id, NULL);
    }

    if (ok) {
        ESP_LOGW(TAG, "AMBIT FLASH SUCCESS (id=%s)", r->id[0] ? r->id : "(none)");
        report_as("ambit_flash_status", "success", r->channel, r->id, NULL);
    } else {
        ESP_LOGE(TAG, "AMBIT FLASH FAILED (id=%s)", r->id[0] ? r->id : "(none)");
        report_as("ambit_flash_status", "failed", r->channel, r->id, NULL);
    }
}

/* Run one queued AMBIT op in the shared maintenance worker (fix #3). Owns and
 * frees `arg` (a heap ambit_ota_req_t). Previously this was a per-op lazy task
 * with a 10 KB stack; that xTaskCreate returned ESP_ERR_NO_MEM on the fragmented
 * field heap, so a remote reflash of a mismatched AMBIT could not launch. The op
 * now runs on the single resident maintenance worker (created when the heap was
 * clean at boot), and dispatch is a tiny job enqueue. */
static void ambit_ota_run(void *arg)
{
    ambit_ota_req_t *r = arg;
    /* Global maintenance gate: refuse to overlap another update type. Redundant
     * under the single shared worker (ops are already serialized), kept as
     * belt-and-suspenders. */
    if (s_cfg.maintenance_begin != NULL && !s_cfg.maintenance_begin()) {
        ESP_LOGW(TAG, "another maintenance op in progress — ambit op=%u id=%s dropped",
                 r->op, r->id[0] ? r->id : "");
        report_busy(r);
        free(r);
        return;
    }
    switch (r->op) {
    case AMBIT_OP_VERSIONS: ambit_do_versions(r->id); break;
    case AMBIT_OP_PROBE:    ambit_do_probe(r);        break;
    case AMBIT_OP_FLASH:    ambit_do_flash(r);        break;
    default:                ambit_do_ota(r);          break;
    }
    if (s_cfg.maintenance_end != NULL) s_cfg.maintenance_end();
    free(r);
}

/* Hand a request to the shared maintenance worker. The heap copy is small
 * (~330 B), so it allocates even on a fragmented heap. */
static esp_err_t ambit_ota_enqueue(const ambit_ota_req_t *r)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    ambit_ota_req_t *copy = malloc(sizeof *copy);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    *copy = *r;
    if (s_cfg.submit == NULL || !s_cfg.submit(ambit_ota_run, copy)) {
        free(copy);                           /* worker queue full — op already queued/in-flight */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ── public API ───────────────────────────────────────────────────────────── */

esp_err_t ambit_ota_init(const ambit_ota_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg   = *cfg;
    s_ready = true;   /* ops dispatch to the shared maintenance worker via s_cfg.submit */
    ESP_LOGI(TAG, "AMBIT OTA ready (shared maintenance worker)");
    return ESP_OK;
}

esp_err_t ambit_ota_request(uint8_t channel, const char *url, const char *id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (channel >= UART_SENSOR_NUM_CHANNELS && channel != AMBIT_OTA_CH_ALL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (url == NULL || url[0] == '\0' || strlen(url) >= AMBIT_OTA_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (id != NULL && strlen(id) >= AMBIT_OTA_ID_MAX) {
        return ESP_ERR_INVALID_ARG;   /* would truncate — the latch could never match it */
    }
    /* Dedupe on the last successfully-applied id (idempotent under a retained
     * MQTT trigger). A NULL id (CLI) or a failed attempt is never deduped. */
    if (already_applied(KEY_APPLIED, id)) {
        ESP_LOGI(TAG, "ambit_ota id=%s already applied — ignoring", id);
        return ESP_OK;
    }
    ambit_ota_req_t r;
    memset(&r, 0, sizeof r);
    r.op      = AMBIT_OP_OTA;
    r.channel = channel;
    strncpy(r.url, url, sizeof r.url - 1);
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    return ambit_ota_enqueue(&r);
}

esp_err_t ambit_ota_report_versions(const char *id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    ambit_ota_req_t r;
    memset(&r, 0, sizeof r);
    r.op = AMBIT_OP_VERSIONS;
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    return ambit_ota_enqueue(&r);
}

esp_err_t ambit_ota_request_probe(uint8_t channel, const char *id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (channel >= UART_SENSOR_NUM_CHANNELS && channel != AMBIT_OTA_CH_ALL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (id != NULL && strlen(id) >= AMBIT_OTA_ID_MAX) return ESP_ERR_INVALID_ARG;
    ambit_ota_req_t r;
    memset(&r, 0, sizeof r);
    r.op      = AMBIT_OP_PROBE;
    r.channel = channel;
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    return ambit_ota_enqueue(&r);   /* identity read: never deduped */
}

esp_err_t ambit_ota_request_flash(uint8_t channel, const char *version, const char *id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (channel >= UART_SENSOR_NUM_CHANNELS && channel != AMBIT_OTA_CH_ALL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Strict canonical <major>.<minor>.<batch> — the trailing %c rejects extra
     * characters, so an MQTT-supplied version can never smuggle path segments;
     * the folder path is rebuilt from the parsed numbers (mirrors ambit_flash's
     * own directory-name parser). */
    unsigned mj = 0, mn = 0, bt = 0;
    char tail = 0;
    if (version == NULL ||
        sscanf(version, "%u.%u.%u%c", &mj, &mn, &bt, &tail) != 3 ||
        mj > 255 || mn > 255 || bt > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    if (id != NULL && strlen(id) >= AMBIT_OTA_ID_MAX) {
        return ESP_ERR_INVALID_ARG;   /* would truncate — the latch could never match it */
    }
    if (already_applied(KEY_APPLIED_FLASH, id)) {
        ESP_LOGI(TAG, "ambit_flash id=%s already applied — ignoring", id);
        return ESP_OK;
    }
    if (flash_fail_count(id) >= AMBIT_FLASH_FAIL_MAX) {
        ESP_LOGE(TAG, "ambit_flash id=%s failed %u times — refusing further retries "
                      "(fix the cause, then retry under a NEW id)",
                 id, AMBIT_FLASH_FAIL_MAX);
        return ESP_ERR_INVALID_STATE;
    }
    ambit_ota_req_t r;
    memset(&r, 0, sizeof r);
    r.op      = AMBIT_OP_FLASH;
    r.channel = channel;
    snprintf(r.url, sizeof r.url, "/sdcard/ambit_fw/%u.%u.%u", mj, mn, bt);
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    return ambit_ota_enqueue(&r);
}

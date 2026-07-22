#include "script_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lua_runner.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sd_card.h"

#define TAG "script_upd"

#define SCRIPT_ID_MAX       64
#define SCRIPT_EXEC_TIMEOUT_MS 120000   /* snippets may run multi-second AMBIT measurements */
#define SCRIPT_RESULT_MAX   192
#define SCRIPT_REBOOT_DELAY_MS 500   /* let the 'applied' reply flush before esp_restart (matches ota_update) */

#define LUA_PATH       "/sdcard/main.lua"
#define LUA_PATH_NEW   "/sdcard/main.lua.new"
#define LUA_PATH_BAK   "/sdcard/main.lua.bak"

#define NVS_NS         "script_upd"
#define KEY_APPLIED    "applied_id"

#define OP_UPDATE     0
#define OP_EXEC       1
#define OP_UPDATE_URL 2

#define SCRIPT_DL_BUF          4096     /* HTTP chunk size — small on purpose (no large contiguous alloc) */
#define SCRIPT_HTTP_TIMEOUT_MS 20000

typedef struct {
    uint8_t op;
    bool    reboot;            /* OP_UPDATE only: reboot after a successful swap (default) */
    char    id[SCRIPT_ID_MAX];
    char    checksum[65];      /* optional sha256 hex ('\0' = absent) */
    char   *text;              /* heap-dup'd script/snippet — freed by the worker */
} script_req_t;

static script_update_config_t s_cfg;
static bool                   s_ready;   /* init done; dispatch via the shared worker (s_cfg.submit) */

/* ── dedupe latch (NVS, success only — same semantics as the OTAs) ────────── */

static void latch_set(const char *id)
{
    if (id == NULL || id[0] == '\0') return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, KEY_APPLIED, id) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static bool already_applied(const char *id)
{
    if (id == NULL || id[0] == '\0') return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char prev[SCRIPT_ID_MAX] = "";
    size_t len = sizeof prev;
    esp_err_t err = nvs_get_str(h, KEY_APPLIED, prev, &len);
    nvs_close(h);
    return err == ESP_OK && strcmp(prev, id) == 0;
}

/* ── reporting ────────────────────────────────────────────────────────────── */

static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    if (in == NULL) { if (cap) out[0] = '\0'; return; }
    for (const char *p = in; *p != '\0' && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\t')        { out[o++] = '\\'; out[o++] = 't'; }
        else if (c >= 0x20)        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

static void publish_json(const char *msg, int n)
{
    if (s_cfg.publish == NULL || s_cfg.status_topic == NULL || s_cfg.status_topic[0] == '\0') {
        return;
    }
    if (s_cfg.is_connected != NULL && !s_cfg.is_connected()) {
        return;   /* console log is authoritative; MQTT report is best-effort */
    }
    if (n > 0) {
        int msg_id = 0;
        s_cfg.publish(s_cfg.status_topic, msg, (size_t)n, &msg_id);
    }
}

static void report_script(const char *state, const char *id, const char *detail)
{
    char esc_id[SCRIPT_ID_MAX * 2 + 1] = "", esc_detail[192] = "";
    json_escape(esc_id, sizeof esc_id, id);
    json_escape(esc_detail, sizeof esc_detail, detail);
    char msg[448];
    int n = snprintf(msg, sizeof msg,
        "{\"type\":\"script_status\",\"device_id\":\"%s\",\"id\":\"%s\",\"state\":\"%s\""
        "%s%s%s}",
        s_cfg.device_id ? s_cfg.device_id : "", esc_id, state,
        detail ? ",\"detail\":\"" : "", esc_detail, detail ? "\"" : "");
    if (n > 0 && (size_t)n < sizeof msg) publish_json(msg, n);
}

static void report_exec(const char *id, bool ok, const char *result)
{
    char esc_id[SCRIPT_ID_MAX * 2 + 1] = "", esc_res[SCRIPT_RESULT_MAX * 2 + 1] = "";
    json_escape(esc_id, sizeof esc_id, id);
    json_escape(esc_res, sizeof esc_res, result);
    char msg[640];
    int n = snprintf(msg, sizeof msg,
        "{\"type\":\"lua_exec_result\",\"device_id\":\"%s\",\"id\":\"%s\",\"ok\":%s,"
        "\"result\":\"%s\"}",
        s_cfg.device_id ? s_cfg.device_id : "", esc_id, ok ? "true" : "false", esc_res);
    if (n > 0 && (size_t)n < sizeof msg) publish_json(msg, n);
}

/* ── OP_UPDATE: replace /sdcard/main.lua ──────────────────────────────────── */

/* SHA-256(text) == checksum (hex, case-insensitive)? */
static bool checksum_ok(const char *text, const char *checksum)
{
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)text, strlen(text), digest, 0) != 0) {
        return false;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    hex[64] = '\0';
    return strncasecmp(hex, checksum, 64) == 0 && strlen(checksum) == 64;
}

/* Parse-only syntax check in a bare state (no env needed — nothing executes). */
static bool syntax_ok(const char *script, char *err, size_t err_cap)
{
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        snprintf(err, err_cap, "out of memory for syntax check");
        return false;
    }
    bool ok = (luaL_loadstring(L, script) == LUA_OK);
    if (!ok) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, err_cap, "%s", msg ? msg : "syntax error");
    }
    lua_close(L);
    return ok;
}

/* Parse-only syntax check straight from a file (url variant — the script is on
 * the SD, not in RAM). luaL_loadfile compiles without executing. */
static bool syntax_ok_file(const char *path, char *err, size_t err_cap)
{
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        snprintf(err, err_cap, "out of memory for syntax check");
        return false;
    }
    bool ok = (luaL_loadfile(L, path) == LUA_OK);
    if (!ok) {
        const char *msg = lua_tostring(L, -1);
        snprintf(err, err_cap, "%s", msg ? msg : "syntax error");
    }
    lua_close(L);
    return ok;
}

/* Stream `url` (HTTPS) into `path`, hashing the bytes as they arrive. 4 KB chunks
 * — never a large contiguous alloc, so this succeeds where an inline 16 KB MQTT
 * message fails on a fragmented heap. On success writes the lowercase-hex SHA-256
 * into hex65[65] and the byte count into *out_size. Removes a partial file on
 * error. Same TLS/buffer settings as the OTA downloaders (cert bundle validates
 * GitHub + CDN; 4 KiB buffers fit long signed-redirect URLs). */
static esp_err_t download_to_file_sha256(const char *url, const char *path,
                                         char hex65[65], size_t *out_size)
{
    *out_size = 0;
    hex65[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = SCRIPT_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size       = SCRIPT_DL_BUF,
        .buffer_size_tx    = SCRIPT_DL_BUF,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }

    int64_t clen   = esp_http_client_fetch_headers(c);
    int     status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGE(TAG, "download HTTP status %d (need 200 — use a direct raw URL, not a /blob/ page)",
                 status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(SCRIPT_DL_BUF);
    if (buf == NULL) {
        fclose(f);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        remove(path);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);   /* 0 = SHA-256 */

    size_t total = 0;
    bool wr_ok = true;
    while (1) {
        int r = esp_http_client_read(c, (char *)buf, SCRIPT_DL_BUF);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;   /* EOF */
        mbedtls_sha256_update(&sha, buf, (size_t)r);
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { err = ESP_ERR_NO_MEM; wr_ok = false; break; }
        total += (size_t)r;
        vTaskDelay(1);       /* yield so the idle task is fed on a fast link */
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (wr_ok) { fflush(f); fsync(fileno(f)); }
    free(buf);
    fclose(f);

    /* Positive completion check: esp_http_client_read()==0 can't distinguish a
     * real EOF from a mid-stream TLS/socket close, and the Content-Length guard
     * below is skipped for chunked responses (clen<=0). Without this, a truncated
     * download whose prefix happens to parse could be installed. */
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(c)) {
        ESP_LOGE(TAG, "incomplete download — connection closed before all data received");
        err = ESP_FAIL;
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && clen > 0 && total != (size_t)clen) {
        ESP_LOGE(TAG, "short download: %u of %lld bytes", (unsigned)total, (long long)clen);
        err = ESP_FAIL;
    }
    if (err != ESP_OK) { remove(path); return err; }

    for (int i = 0; i < 32; i++) sprintf(hex65 + i * 2, "%02x", digest[i]);
    hex65[64] = '\0';
    *out_size = total;
    return ESP_OK;
}

static void do_update(const script_req_t *r)
{
    const size_t len = strlen(r->text);
    ESP_LOGW(TAG, "script_update id=%s: %u bytes", r->id[0] ? r->id : "(none)", (unsigned)len);
    char detail[160];

    /* On-receipt ack: tell the operator the command was received before doing any
     * work (syntax check + SD write + Lua stop can take a few seconds, and the
     * fleet-OTA notebook waits for an initial reply). MQTT stays up on the inline
     * path, so this lands immediately. */
    report_script("accepted", r->id, NULL);

    if (r->checksum[0] != '\0' && !checksum_ok(r->text, r->checksum)) {
        ESP_LOGE(TAG, "checksum mismatch — script rejected");
        report_script("failed", r->id, "sha256 mismatch");
        return;
    }
    if (!syntax_ok(r->text, detail, sizeof detail)) {
        ESP_LOGE(TAG, "syntax check failed: %s — main.lua untouched", detail);
        report_script("failed", r->id, detail);
        return;
    }
    if (!sdcard_is_mounted() && sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD not available");
        report_script("failed", r->id, "SD card not mounted");
        return;
    }

    /* Stage the new script next to the live one, fully flushed before any swap. */
    FILE *f = fopen(LUA_PATH_NEW, "wb");
    if (f == NULL) {
        report_script("failed", r->id, "cannot open " LUA_PATH_NEW);
        return;
    }
    bool wr_ok = (fwrite(r->text, 1, len, f) == len);
    if (wr_ok) { fflush(f); fsync(fileno(f)); }
    fclose(f);
    if (!wr_ok) {
        remove(LUA_PATH_NEW);
        report_script("failed", r->id, "SD write failed");
        return;
    }

    /* Stop the runner before the swap; a stop timeout means the script is wedged
     * in a long C call — leave everything untouched and let the operator retry. */
    if (lua_runner_stop(5000) == ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "lua task still busy — not swapping; retry in a moment");
        report_script("failed", r->id, "lua task busy; retry");
        return;
    }

    /* Swap: previous script survives as main.lua.bak (manual recovery path).
     * rename() is atomic on FATFS; a missing old main.lua (first install) is fine. */
    remove(LUA_PATH_BAK);
    (void)rename(LUA_PATH, LUA_PATH_BAK);
    if (rename(LUA_PATH_NEW, LUA_PATH) != 0) {
        ESP_LOGE(TAG, "rename to %s failed — restarting the old script", LUA_PATH);
        (void)rename(LUA_PATH_BAK, LUA_PATH);   /* best-effort restore */
        (void)lua_runner_start();
        report_script("failed", r->id, "rename failed");
        return;
    }

    /* Reboot path (default): the new main.lua is on the SD and the runner is
     * already stopped — a full restart runs it from a fresh boot (clean heap,
     * ordered startup). Latch FIRST so a retained trigger dedupes on reconnect
     * and can't loop the reboot (same guard as ota_update). */
    if (r->reboot) {
        latch_set(r->id);
        ESP_LOGW(TAG, "main.lua replaced (%u bytes); previous kept as %s — rebooting to run it",
                 (unsigned)len, LUA_PATH_BAK);
        snprintf(detail, sizeof detail, "%u bytes; rebooting", (unsigned)len);
        report_script("applied", r->id, detail);
        vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));   /* flush the MQTT reply */
        esp_restart();                                       /* no return */
    }

    /* In-place path (reboot=false): restart just the Lua runner on the new file. */
    esp_err_t err = lua_runner_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runner restart failed: %s (script IS installed — `lua start` manually)",
                 esp_err_to_name(err));
        report_script("failed", r->id, "script installed but runner restart failed");
        return;
    }

    latch_set(r->id);
    ESP_LOGW(TAG, "main.lua replaced (%u bytes) + runner restarted; previous kept as %s",
             (unsigned)len, LUA_PATH_BAK);
    snprintf(detail, sizeof detail, "%u bytes", (unsigned)len);
    report_script("applied", r->id, detail);
}

/* ── OP_UPDATE_URL: download /sdcard/main.lua from a URL ───────────────────────
 * The command message is tiny (just the URL), so it's received even on a
 * fragmented heap; the heavy transfer is a chunked HTTPS download AFTER Lua is
 * stopped (heap defragmented). This is the reliable path for large scripts —
 * inline 16 KB MQTT delivery needs a contiguous TLS record buffer the fragmented
 * heap can't provide. `r->text` holds the URL. */
static void do_update_url(const script_req_t *r)
{
    ESP_LOGW(TAG, "script_update(url) id=%s: %s", r->id[0] ? r->id : "(none)", r->text);
    char detail[192] = "";

    if (s_cfg.workload_suspend == NULL || s_cfg.workload_resume == NULL) {
        ESP_LOGE(TAG, "url variant needs workload hooks — not configured");
        report_script("failed", r->id, "url variant unavailable");
        return;
    }

    /* On-receipt ack — MUST publish BEFORE comms_suspend() below, after which MQTT
     * is gone and the operator would otherwise see nothing until the terminal
     * report minutes later (or never, if the reconnect drops it). Flush before the
     * comms drop, mirroring the OTA path's accepted-ack flush. */
    report_script("accepted", r->id, NULL);
    vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));

    /* Quiesce like the OTAs: stop Lua (frees its 8 KB buffer + UART, defragments)
     * AND stop MQTT (frees its TLS heap) so the download's HTTPS handshake gets a
     * clean, contiguous heap on this PSRAM-less board. MQTT is resumed before we
     * report. */
    s_cfg.workload_suspend();
    if (s_cfg.comms_suspend != NULL) s_cfg.comms_suspend();

    bool applied = false;
    size_t n = 0;
    char got[65] = "";

    if (!sdcard_is_mounted() && sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD not available");
        snprintf(detail, sizeof detail, "SD card not mounted");
    } else {
        esp_err_t err = download_to_file_sha256(r->text, LUA_PATH_NEW, got, &n);
        if (err != ESP_OK) {
            snprintf(detail, sizeof detail, "download failed (%s)", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", detail);
        } else if (r->checksum[0] != '\0' &&
                   (strlen(r->checksum) != 64 || strncasecmp(got, r->checksum, 64) != 0)) {
            ESP_LOGE(TAG, "checksum mismatch — script rejected");
            remove(LUA_PATH_NEW);
            snprintf(detail, sizeof detail, "sha256 mismatch");
        } else if (!syntax_ok_file(LUA_PATH_NEW, detail, sizeof detail)) {
            ESP_LOGE(TAG, "syntax check failed: %s — main.lua untouched", detail);
            remove(LUA_PATH_NEW);
        } else {
            ESP_LOGW(TAG, "downloaded %u bytes, sha256=%s", (unsigned)n, got);
            /* Lua already stopped; swap (previous kept as .bak; atomic on FATFS). */
            remove(LUA_PATH_BAK);
            (void)rename(LUA_PATH, LUA_PATH_BAK);
            if (rename(LUA_PATH_NEW, LUA_PATH) != 0) {
                ESP_LOGE(TAG, "rename to %s failed", LUA_PATH);
                (void)rename(LUA_PATH_BAK, LUA_PATH);   /* best-effort restore */
                snprintf(detail, sizeof detail, "rename failed");
            } else {
                latch_set(r->id);   /* before any reboot: dedupes a retained trigger */
                applied = true;
            }
        }
    }

    /* Bring MQTT back and give it a moment to reconnect so the status reply lands
     * (best-effort — the serial log is authoritative either way). */
    if (s_cfg.comms_resume != NULL) {
        s_cfg.comms_resume();
        for (int i = 0; i < 3000 && s_cfg.is_connected != NULL && !s_cfg.is_connected(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (!applied) {
        report_script("failed", r->id, detail);
        s_cfg.workload_resume();   /* restart the old script */
        return;
    }

    if (r->reboot) {
        ESP_LOGW(TAG, "main.lua replaced from url (%u bytes); previous kept as %s — rebooting to run it",
                 (unsigned)n, LUA_PATH_BAK);
        snprintf(detail, sizeof detail, "%u bytes; rebooting", (unsigned)n);
        report_script("applied", r->id, detail);
        vTaskDelay(pdMS_TO_TICKS(SCRIPT_REBOOT_DELAY_MS));   /* flush the MQTT reply */
        esp_restart();                                       /* no return */
    }

    /* In-place: restart the Lua runner on the new script. */
    s_cfg.workload_resume();
    ESP_LOGW(TAG, "main.lua replaced from url (%u bytes) + runner restarted; previous kept as %s",
             (unsigned)n, LUA_PATH_BAK);
    snprintf(detail, sizeof detail, "%u bytes", (unsigned)n);
    report_script("applied", r->id, detail);
}

/* ── OP_EXEC: run a snippet now ───────────────────────────────────────────── */

static void do_exec(const script_req_t *r)
{
    ESP_LOGW(TAG, "lua_exec id=%s: %u bytes", r->id[0] ? r->id : "(none)",
             (unsigned)strlen(r->text));
    char result[SCRIPT_RESULT_MAX] = "";
    esp_err_t err = lua_runner_exec(r->text, SCRIPT_EXEC_TIMEOUT_MS, result, sizeof result);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "lua_exec ok: %s", result[0] ? result : "(no return value)");
    } else {
        ESP_LOGE(TAG, "lua_exec failed (%s): %s", esp_err_to_name(err), result);
    }
    report_exec(r->id, err == ESP_OK, result);
}

/* ── shared maintenance worker dispatch (fix #3) ──────────────────────────── */

/* Run one queued script/exec op in the shared maintenance worker. Owns and frees
 * `arg` (a heap script_req_t) AND its ->text. Previously this ran on a per-op
 * lazy task with a 10 KB stack, whose xTaskCreate failed (ESP_ERR_NO_MEM) on the
 * fragmented field heap — so a remote main.lua push (the primary recovery path)
 * could not launch. It now runs on the single resident maintenance worker. */
static void script_run(void *arg)
{
    script_req_t *r = arg;
    /* Global maintenance gate: refuse to overlap another update type. Redundant
     * under the single shared worker (ops are already serialized), kept as
     * belt-and-suspenders. */
    if (s_cfg.maintenance_begin != NULL && !s_cfg.maintenance_begin()) {
        ESP_LOGW(TAG, "another maintenance op in progress — script op=%u id=%s dropped",
                 r->op, r->id[0] ? r->id : "(none)");
        if (r->op == OP_EXEC) report_exec(r->id, false, "device busy (maintenance in progress)");
        else                  report_script("busy", r->id, "another maintenance op is in progress");
        free(r->text);
        free(r);
        return;
    }
    if (r->op == OP_EXEC)            do_exec(r);
    else if (r->op == OP_UPDATE_URL) do_update_url(r);
    else                            do_update(r);
    if (s_cfg.maintenance_end != NULL) s_cfg.maintenance_end();
    free(r->text);
    free(r);
}

/* Hand a request to the shared maintenance worker. Takes ownership of r->text on
 * success (the worker frees it); on failure the caller (request_common) frees it.
 * The heap copy is small (~350 B), so it allocates even on a fragmented heap. */
static esp_err_t enqueue(script_req_t *r)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    script_req_t *copy = malloc(sizeof *copy);
    if (copy == NULL) return ESP_ERR_NO_MEM;   /* caller frees r->text */
    *copy = *r;                                /* copy->text aliases r->text */
    if (s_cfg.submit == NULL || !s_cfg.submit(script_run, copy)) {
        free(copy);                            /* NOT copy->text — caller still owns it */
        return ESP_ERR_NO_MEM;                 /* worker queue full — op already queued/in-flight */
    }
    return ESP_OK;                             /* worker now owns copy + copy->text */
}

/* ── public API ───────────────────────────────────────────────────────────── */

esp_err_t script_update_init(const script_update_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg   = *cfg;
    s_ready = true;   /* ops dispatch to the shared maintenance worker via s_cfg.submit */
    ESP_LOGI(TAG, "script/exec module ready (shared maintenance worker)");
    return ESP_OK;
}

static esp_err_t request_common(uint8_t op, const char *text,
                                const char *checksum, const char *id, bool reboot)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (text == NULL || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    script_req_t r;
    memset(&r, 0, sizeof r);
    r.op = op;
    r.reboot = reboot;
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    if (checksum != NULL) strncpy(r.checksum, checksum, sizeof r.checksum - 1);
    r.text = strdup(text);
    if (r.text == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = enqueue(&r);
    if (err != ESP_OK) free(r.text);
    return err;
}

/* A reboot with no id can't be deduped (latch_set/already_applied are no-ops for
 * an empty id), so a RETAINED reboot=true command would re-apply + reboot on every
 * reconnect — a boot loop. Require an id to reboot. */
static bool reboot_needs_id(const char *id, bool reboot, const char *what)
{
    if (reboot && (id == NULL || id[0] == '\0')) {
        ESP_LOGW(TAG, "%s reboot=true requires an id — rejecting (retained-loop guard)", what);
        report_script("failed", id, "reboot requires an id");
        return true;
    }
    return false;
}

esp_err_t script_update_request(const char *script, const char *checksum, const char *id,
                                bool reboot)
{
    if (reboot_needs_id(id, reboot, "script_update")) return ESP_ERR_INVALID_ARG;
    /* Retained-topic dedupe: an already-applied id is ignored (success-latched). */
    if (already_applied(id)) {
        ESP_LOGI(TAG, "script_update id=%s already applied — ignoring", id);
        return ESP_OK;
    }
    return request_common(OP_UPDATE, script, checksum, id, reboot);
}

esp_err_t script_update_url_request(const char *url, const char *checksum, const char *id,
                                    bool reboot)
{
    if (reboot_needs_id(id, reboot, "script_update(url)")) return ESP_ERR_INVALID_ARG;
    /* Same success-latch dedupe as the inline path (stops a retained url command
     * from re-downloading + re-rebooting on every reconnect). */
    if (already_applied(id)) {
        ESP_LOGI(TAG, "script_update(url) id=%s already applied — ignoring", id);
        return ESP_OK;
    }
    return request_common(OP_UPDATE_URL, url, checksum, id, reboot);   /* text holds the URL */
}

esp_err_t script_update_exec_request(const char *code, const char *id)
{
    return request_common(OP_EXEC, code, NULL, id, false);   /* exec is never deduped/rebooted */
}

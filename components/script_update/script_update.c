#include "script_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
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

#define SCRIPT_TASK_STACK   8192
#define SCRIPT_TASK_PRIO    4        /* below lua_runner(10); admin work, not latency-critical */
#define SCRIPT_IDLE_EXIT_MS 2000     /* free the worker stack this long after idle */
#define SCRIPT_ID_MAX       64
#define SCRIPT_EXEC_TIMEOUT_MS 120000   /* snippets may run multi-second AMBIT measurements */
#define SCRIPT_RESULT_MAX   192

#define LUA_PATH       "/sdcard/main.lua"
#define LUA_PATH_NEW   "/sdcard/main.lua.new"
#define LUA_PATH_BAK   "/sdcard/main.lua.bak"

#define NVS_NS         "script_upd"
#define KEY_APPLIED    "applied_id"

#define OP_UPDATE 0
#define OP_EXEC   1

typedef struct {
    uint8_t op;
    char    id[SCRIPT_ID_MAX];
    char    checksum[65];      /* optional sha256 hex ('\0' = absent) */
    char   *text;              /* heap-dup'd script/snippet — freed by the worker */
} script_req_t;

static script_update_config_t s_cfg;
static QueueHandle_t          s_queue;
static TaskHandle_t           s_task;   /* NULL when no worker is running (lazy) */
static SemaphoreHandle_t      s_lock;   /* guards s_task lifecycle vs enqueue */

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

static void do_update(const script_req_t *r)
{
    const size_t len = strlen(r->text);
    ESP_LOGW(TAG, "script_update id=%s: %u bytes", r->id[0] ? r->id : "(none)", (unsigned)len);
    char detail[160];

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

/* ── lazy worker (same shape as ambit_ota: zero steady-state heap) ────────── */

static void script_task(void *arg)
{
    (void)arg;
    script_req_t r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, pdMS_TO_TICKS(SCRIPT_IDLE_EXIT_MS)) == pdTRUE) {
            if (r.op == OP_EXEC) do_exec(&r);
            else                 do_update(&r);
            free(r.text);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (uxQueueMessagesWaiting(s_queue) == 0) {
            s_task = NULL;
            xSemaphoreGive(s_lock);
            vTaskDelete(NULL);   /* idle — give the stack back */
        }
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t enqueue(script_req_t *r)
{
    if (s_queue == NULL || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (xQueueSend(s_queue, r, 0) != pdTRUE) {
        ret = ESP_ERR_NO_MEM;                 /* an op is already queued/in-flight */
    } else if (s_task == NULL) {
        if (xTaskCreatePinnedToCore(script_task, "script_upd", SCRIPT_TASK_STACK,
                                    NULL, SCRIPT_TASK_PRIO, &s_task, 0) != pdPASS) {
            script_req_t drop;
            xQueueReceive(s_queue, &drop, 0);   /* undo — no worker to run it */
            s_task = NULL;
            ret = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s_lock);
    return ret;
}

/* ── public API ───────────────────────────────────────────────────────────── */

esp_err_t script_update_init(const script_update_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    if (s_queue != NULL) return ESP_OK;   /* idempotent */

    s_lock  = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(2, sizeof(script_req_t));
    if (s_lock == NULL || s_queue == NULL) {
        if (s_lock)  vSemaphoreDelete(s_lock);
        if (s_queue) vQueueDelete(s_queue);
        s_lock = NULL;
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "script/exec worker ready (spawned on demand)");
    return ESP_OK;
}

static esp_err_t request_common(uint8_t op, const char *text,
                                const char *checksum, const char *id)
{
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (text == NULL || text[0] == '\0') return ESP_ERR_INVALID_ARG;

    script_req_t r;
    memset(&r, 0, sizeof r);
    r.op = op;
    if (id != NULL) strncpy(r.id, id, sizeof r.id - 1);
    if (checksum != NULL) strncpy(r.checksum, checksum, sizeof r.checksum - 1);
    r.text = strdup(text);
    if (r.text == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = enqueue(&r);
    if (err != ESP_OK) free(r.text);
    return err;
}

esp_err_t script_update_request(const char *script, const char *checksum, const char *id)
{
    /* Retained-topic dedupe: an already-applied id is ignored (success-latched). */
    if (already_applied(id)) {
        ESP_LOGI(TAG, "script_update id=%s already applied — ignoring", id);
        return ESP_OK;
    }
    return request_common(OP_UPDATE, script, checksum, id);
}

esp_err_t script_update_exec_request(const char *code, const char *id)
{
    return request_common(OP_EXEC, code, NULL, id);   /* exec is never deduped */
}

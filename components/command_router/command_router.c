#include "command_router.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ota_update.h"
#include "ambit_ota.h"
#include "script_update.h"

#define TAG "cmd_router"

static command_router_config_t s_cfg;   /* pointers reference app_main's static buffers */

/* Idempotency lives in ota_update now: it latches an id only on a *successful*
 * update (set-boot done), so a retained/duplicate trigger is ignored while a
 * failed attempt stays retryable under the same id. The router just forwards. */

static void publish_reply(const char *json)
{
    if (s_cfg.publish == NULL || s_cfg.status_topic == NULL || s_cfg.status_topic[0] == '\0') {
        return;
    }
    int msg_id = 0;
    s_cfg.publish(s_cfg.status_topic, json, strlen(json), &msg_id);
}

static void handle_ping(const char *id)
{
    char reply[256];
    const long long up_ms = (long long)(esp_timer_get_time() / 1000);
    snprintf(reply, sizeof(reply),
             "{\"type\":\"pong\",\"id\":\"%s\",\"device_id\":\"%s\",\"fw\":\"%s\",\"uptime_ms\":%lld}",
             id ? id : "",
             s_cfg.device_id ? s_cfg.device_id : "",
             s_cfg.firmware_version ? s_cfg.firmware_version : "",
             up_ms);
    ESP_LOGI(TAG, "ping -> pong (id=%s)", id ? id : "");
    publish_reply(reply);
}

/* message_received_fn — runs in the mqtt task. Keep light; hand long work (OTA) to
 * a separate task in Stage 3. */
static void on_message(const char *topic, const char *payload, size_t len, void *ctx)
{
    (void)topic;
    (void)len;
    (void)ctx;

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "command JSON parse failed");
        return;
    }

    const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *jid   = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *type = cJSON_IsString(jtype) ? jtype->valuestring : NULL;
    const char *id   = cJSON_IsString(jid)   ? jid->valuestring   : NULL;

    if (type == NULL) {
        ESP_LOGW(TAG, "command missing 'type'");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "command type=%s id=%s", type, id ? id : "(none)");

    if (strcmp(type, "ping") == 0) {
        handle_ping(id);
    } else if (strcmp(type, "ota_update") == 0) {
        const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");
        const char *url = cJSON_IsString(jurl) ? jurl->valuestring : NULL;
        if (url == NULL) {
            ESP_LOGW(TAG, "ota_update id=%s missing 'url' — ignoring", id ? id : "");
        } else {
            /* ota_update owns dedupe (on success) + the download/reboot. */
            esp_err_t err = ota_update_request(url, id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ota_update id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "ota_update id=%s dispatched (url=%s)", id ? id : "", url);
            }
        }
    } else if (strcmp(type, "ambit_ota") == 0) {
        /* Stream a new AMBIT (C3) firmware image over UART. {url, channel}:
         * channel 0-3 = one sensor; "all" or a negative number = every channel.
         * ambit_ota owns dedupe (on success) + the download/stream/reboot. */
        const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *jch  = cJSON_GetObjectItemCaseSensitive(root, "channel");
        const char *url = cJSON_IsString(jurl) ? jurl->valuestring : NULL;
        uint8_t ch = 0;
        bool ch_ok = true;
        if (cJSON_IsNumber(jch) && jch->valueint >= 0 && jch->valueint < 4) {
            ch = (uint8_t)jch->valueint;
        } else if (cJSON_IsNumber(jch) && jch->valueint < 0) {
            ch = AMBIT_OTA_CH_ALL;
        } else if (cJSON_IsString(jch) && strcmp(jch->valuestring, "all") == 0) {
            ch = AMBIT_OTA_CH_ALL;
        } else {
            ch_ok = false;
        }
        if (url == NULL) {
            ESP_LOGW(TAG, "ambit_ota id=%s missing 'url' — ignoring", id ? id : "");
        } else if (!ch_ok) {
            ESP_LOGW(TAG, "ambit_ota id=%s bad/missing 'channel' (0-3 or \"all\") — ignoring",
                     id ? id : "");
        } else {
            esp_err_t err = ambit_ota_request(ch, url, id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ambit_ota id=%s dispatch failed: %s", id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "ambit_ota id=%s dispatched (ch=%u url=%s)", id ? id : "", ch, url);
            }
        }
    } else if (strcmp(type, "ambit_probe") == 0) {
        /* ROM-bootloader probe (chip + MAC) — works on bricked/blank AMBITs.
         * {channel: 0-3 | "all" | negative}; absent = all. Publishes one
         * ambit_probe report on the status topic. */
        uint8_t ch = AMBIT_OTA_CH_ALL;
        const cJSON *jch = cJSON_GetObjectItemCaseSensitive(root, "channel");
        bool ch_ok = true;
        if (jch != NULL) {
            /* Integers only: 3.7 must be rejected, not silently truncated to 3. */
            if (cJSON_IsNumber(jch) && jch->valuedouble == (double)jch->valueint) {
                if (jch->valueint >= 0 && jch->valueint < 4) ch = (uint8_t)jch->valueint;
                else if (jch->valueint < 0)                  ch = AMBIT_OTA_CH_ALL;
                else                                         ch_ok = false;
            } else if (!(cJSON_IsString(jch) && strcmp(jch->valuestring, "all") == 0)) {
                ch_ok = false;
            }
        }
        if (!ch_ok) {
            ESP_LOGW(TAG, "ambit_probe id=%s bad 'channel' (0-3 or \"all\") — ignoring",
                     id ? id : "");
        } else {
            esp_err_t err = ambit_ota_request_probe(ch, id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ambit_probe id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "ambit_probe id=%s dispatched (ch=%u)", id ? id : "", ch);
            }
        }
    } else if (strcmp(type, "ambit_flash") == 0) {
        /* Full 4-region ROM flash from /sdcard/ambit_fw/<version>/ (Strategy A —
         * revives bricked/pre-OTA units; AMBIT NVS/calibration preserved).
         * {version:"M.m.b", channel: 0-3 | "all" | negative}; absent channel =
         * all ROM-answering channels. The SD must already hold the folder —
         * this path does not download. ambit_ota owns the id dedupe latch. */
        const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
        const char *version = cJSON_IsString(jver) ? jver->valuestring : NULL;
        uint8_t ch = AMBIT_OTA_CH_ALL;
        const cJSON *jch = cJSON_GetObjectItemCaseSensitive(root, "channel");
        bool ch_ok = true;
        if (jch != NULL) {
            /* Integers only: 3.7 must be rejected, not silently truncated to 3. */
            if (cJSON_IsNumber(jch) && jch->valuedouble == (double)jch->valueint) {
                if (jch->valueint >= 0 && jch->valueint < 4) ch = (uint8_t)jch->valueint;
                else if (jch->valueint < 0)                  ch = AMBIT_OTA_CH_ALL;
                else                                         ch_ok = false;
            } else if (!(cJSON_IsString(jch) && strcmp(jch->valuestring, "all") == 0)) {
                ch_ok = false;
            }
        }
        if (version == NULL) {
            ESP_LOGW(TAG, "ambit_flash id=%s missing 'version' — ignoring", id ? id : "");
        } else if (!ch_ok) {
            ESP_LOGW(TAG, "ambit_flash id=%s bad 'channel' (0-3 or \"all\") — ignoring",
                     id ? id : "");
        } else {
            esp_err_t err = ambit_ota_request_flash(ch, version, id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ambit_flash id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "ambit_flash id=%s dispatched (ch=%u ver=%s)",
                         id ? id : "", ch, version);
            }
        }
    } else if (strcmp(type, "ambit_versions") == 0) {
        /* Sweep every channel's AMBIT firmware version → one ambit_versions
         * report on the status topic. Runs on the ambit_ota worker, off this
         * (MQTT) task. */
        esp_err_t err = ambit_ota_report_versions(id);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ambit_versions id=%s dispatch failed: %s", id ? id : "", esp_err_to_name(err));
        }
    } else if (strcmp(type, "script_update") == 0) {
        /* Replace /sdcard/main.lua with the inline script. `script` is the
         * canonical field; `payload` is the legacy name from
         * device-script-delivery.md. Optional `checksum` = sha256 hex. Optional
         * `reboot` (bool, default true) restarts the whole device after a
         * successful swap so the new script runs from a fresh boot; set it false
         * to keep the in-place swap + Lua-runner restart. */
        const cJSON *jscript = cJSON_GetObjectItemCaseSensitive(root, "script");
        if (!cJSON_IsString(jscript)) {
            jscript = cJSON_GetObjectItemCaseSensitive(root, "payload");
        }
        const cJSON *jsum = cJSON_GetObjectItemCaseSensitive(root, "checksum");
        const cJSON *jreboot = cJSON_GetObjectItemCaseSensitive(root, "reboot");
        const char *script   = cJSON_IsString(jscript) ? jscript->valuestring : NULL;
        const char *checksum = cJSON_IsString(jsum)    ? jsum->valuestring    : NULL;
        bool reboot = true;   /* default: reboot into the new script */
        if (cJSON_IsBool(jreboot)) reboot = cJSON_IsTrue(jreboot);
        if (script == NULL || script[0] == '\0') {
            ESP_LOGW(TAG, "script_update id=%s missing 'script' — ignoring", id ? id : "");
        } else {
            esp_err_t err = script_update_request(script, checksum, id, reboot);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "script_update id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "script_update id=%s dispatched (%u bytes, %s)",
                         id ? id : "", (unsigned)strlen(script), reboot ? "reboot" : "in-place");
            }
        }
    } else if (strcmp(type, "lua_exec") == 0) {
        /* Run a Lua snippet immediately (ephemeral state, alongside main.lua);
         * the result publishes as lua_exec_result on the status topic. */
        const cJSON *jcode = cJSON_GetObjectItemCaseSensitive(root, "code");
        const char *code = cJSON_IsString(jcode) ? jcode->valuestring : NULL;
        if (code == NULL || code[0] == '\0') {
            ESP_LOGW(TAG, "lua_exec id=%s missing 'code' — ignoring", id ? id : "");
        } else {
            esp_err_t err = script_update_exec_request(code, id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "lua_exec id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            }
        }
    } else {
        ESP_LOGW(TAG, "unknown command type '%s'", type);
    }

    cJSON_Delete(root);
}

esp_err_t command_router_init(const command_router_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    ESP_LOGI(TAG, "command router ready (status_topic=%s)",
             s_cfg.status_topic ? s_cfg.status_topic : "(none)");
    return ESP_OK;
}

message_received_fn command_router_get_received_fn(void)
{
    return on_message;
}

#include "command_router.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ota_update.h"
#include "ambit_ota.h"
#include "script_update.h"
#include "sched_runner.h"
#include "device_commands.h"
#include "device_config.h"
#include "time_sync.h"

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
            esp_err_t err = ambit_ota_request(ch, url, id, true /* remote fleet spread */);
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
        /* Replace /littlefs/schedule.yaml. Two delivery modes:
         *   - `url`   : download the script over HTTPS (reliable on a fragmented
         *               heap — tiny command, chunked download). Preferred for big
         *               scripts. `checksum` = sha256 hex of the fetched file.
         *   - `script`: inline (legacy alias `payload`). Capped at the 16 KB MQTT
         *               message and needs a contiguous TLS buffer to be received.
         * Optional `reboot` (bool, default true) restarts the device after a
         * successful swap; false keeps the in-place schedule-runner restart. */
        const cJSON *jscript = cJSON_GetObjectItemCaseSensitive(root, "script");
        if (!cJSON_IsString(jscript)) {
            jscript = cJSON_GetObjectItemCaseSensitive(root, "payload");
        }
        const cJSON *jurl    = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *jsum    = cJSON_GetObjectItemCaseSensitive(root, "checksum");
        const cJSON *jreboot = cJSON_GetObjectItemCaseSensitive(root, "reboot");
        const cJSON *jver    = cJSON_GetObjectItemCaseSensitive(root, "script_version");
        const cJSON *jbuilt  = cJSON_GetObjectItemCaseSensitive(root, "built_against_fw");
        const char *script   = cJSON_IsString(jscript) ? jscript->valuestring : NULL;
        const char *url      = cJSON_IsString(jurl)    ? jurl->valuestring    : NULL;
        const char *checksum = cJSON_IsString(jsum)    ? jsum->valuestring    : NULL;
        const char *script_version = cJSON_IsString(jver) ? jver->valuestring : NULL;
        const char *built_against_fw = cJSON_IsString(jbuilt) ? jbuilt->valuestring : NULL;
        bool reboot = true;   /* default: reboot into the new script */
        if (cJSON_IsBool(jreboot)) reboot = cJSON_IsTrue(jreboot);
        if (url != NULL && url[0] != '\0') {
            esp_err_t err = script_update_url_request(url, checksum, id, reboot,
                                                      script_version, built_against_fw);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "script_update(url) id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "script_update(url) id=%s dispatched (%s, %s)",
                         id ? id : "", url, reboot ? "reboot" : "in-place");
            }
        } else if (script == NULL || script[0] == '\0') {
            ESP_LOGW(TAG, "script_update id=%s missing 'script'/'url' — ignoring", id ? id : "");
        } else {
            esp_err_t err = script_update_request(script, checksum, id, reboot,
                                                  script_version, built_against_fw);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "script_update id=%s dispatch failed: %s",
                         id ? id : "", esp_err_to_name(err));
            } else {
                ESP_LOGW(TAG, "script_update id=%s dispatched (%u bytes, %s)",
                         id ? id : "", (unsigned)strlen(script), reboot ? "reboot" : "in-place");
            }
        }
    } else if (strcmp(type, "schedule_run") == 0) {
        /* Dispatch a schedule job on demand: {"job": "<name>"}. The runner
         * executes it on its own task (sequential with scheduled jobs); this
         * only enqueues, so it is safe on the MQTT task. The reply is a
         * schedule_run_result on the status topic. Built with cJSON: the job
         * name is attacker-controlled MQTT input and must be JSON-escaped,
         * not interpolated (a quote in it would tear the reply). */
        const cJSON *jjob = cJSON_GetObjectItemCaseSensitive(root, "job");
        const char *job = cJSON_IsString(jjob) ? jjob->valuestring : NULL;
        if (job == NULL || job[0] == '\0') {
            ESP_LOGW(TAG, "schedule_run id=%s missing 'job' — ignoring", id ? id : "");
        } else {
            esp_err_t err = sched_runner_dispatch(job);
            ESP_LOGW(TAG, "schedule_run id=%s job=%s -> %s", id ? id : "", job,
                     esp_err_to_name(err));
            cJSON *reply = cJSON_CreateObject();
            if (reply != NULL) {
                cJSON_AddStringToObject(reply, "type", "schedule_run_result");
                cJSON_AddStringToObject(reply, "id", id ? id : "");
                cJSON_AddBoolToObject(reply, "ok", err == ESP_OK);
                cJSON_AddStringToObject(reply, "job", job);
                cJSON_AddStringToObject(reply, "detail", esp_err_to_name(err));
                char *json = cJSON_PrintUnformatted(reply);
                if (json != NULL) {
                    publish_reply(json);
                    cJSON_free(json);
                }
                cJSON_Delete(reply);
            }
        }
    } else if (strcmp(type, "set_location") == 0) {
        /* Persist and apply site position. Safe as a retained command: unlike
         * set_time these values do not become stale while a device is offline. */
        const cJSON *jlat = cJSON_GetObjectItemCaseSensitive(root, "lat");
        const cJSON *jlon = cJSON_GetObjectItemCaseSensitive(root, "lon");
        const cJSON *jdeployment = cJSON_GetObjectItemCaseSensitive(root, "deployment");
        const char *deployment = cJSON_IsString(jdeployment) ? jdeployment->valuestring : NULL;
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon) &&
            isfinite(jlat->valuedouble) && isfinite(jlon->valuedouble) &&
            jlat->valuedouble >= -90.0 && jlat->valuedouble <= 90.0 &&
            jlon->valuedouble >= -180.0 && jlon->valuedouble <= 180.0 &&
            (jdeployment == NULL || cJSON_IsString(jdeployment))) {
            err = device_config_set_location(jlat->valuedouble,
                                             jlon->valuedouble,
                                             deployment);
            if (err == ESP_OK) {
                int tz = 0;
                time_sync_get_location(NULL, NULL, &tz);
                time_sync_set_location(jlat->valuedouble, jlon->valuedouble, tz);
            }
        }
        ESP_LOGW(TAG, "set_location id=%s -> %s", id ? id : "", esp_err_to_name(err));
        char applied_deployment[64] = "";
        (void)device_config_get_deployment(applied_deployment,
                                           sizeof(applied_deployment));
        cJSON *reply = cJSON_CreateObject();
        if (reply != NULL) {
            cJSON_AddStringToObject(reply, "type", "set_location_result");
            cJSON_AddStringToObject(reply, "id", id ? id : "");
            cJSON_AddBoolToObject(reply, "ok", err == ESP_OK);
            cJSON_AddNumberToObject(reply, "lat",
                cJSON_IsNumber(jlat) && isfinite(jlat->valuedouble)
                    ? jlat->valuedouble : 0.0);
            cJSON_AddNumberToObject(reply, "lon",
                cJSON_IsNumber(jlon) && isfinite(jlon->valuedouble)
                    ? jlon->valuedouble : 0.0);
            cJSON_AddStringToObject(reply, "deployment", applied_deployment);
            cJSON_AddStringToObject(reply, "detail", esp_err_to_name(err));
            char *encoded = cJSON_PrintUnformatted(reply);
            if (encoded != NULL) {
                publish_reply(encoded);
                cJSON_free(encoded);
            }
            cJSON_Delete(reply);
        }
    } else if (strcmp(type, "set_time") == 0) {
        /* Set the device RTC from a UTC epoch: {"epoch": <UTC seconds>}. The RTC is
         * UTC by design — send UTC, never local. Do NOT publish set_time as a
         * retained message: the sender-stamped epoch would replay stale on a late
         * reconnect (cmd_set_rtc range-checks but cannot detect a merely-old stamp).
         * Parse via valuedouble (not valueint) so epochs past 2038 don't overflow. */
        const cJSON *jepoch = cJSON_GetObjectItemCaseSensitive(root, "epoch");
        if (!cJSON_IsNumber(jepoch)) {
            ESP_LOGW(TAG, "set_time id=%s missing/invalid 'epoch' — ignoring", id ? id : "");
        } else {
            int64_t epoch = (int64_t)jepoch->valuedouble;
            cmd_result_t r = cmd_set_rtc(epoch);
            ESP_LOGW(TAG, "set_time id=%s epoch=%lld -> %s (%s)", id ? id : "",
                     (long long)epoch, r.status == ESP_OK ? "OK" : "FAIL", r.message);
            char reply[384];
            /* Bound id and detail so the fixed buffer can never truncate (r.message
             * is char[256]; id is caller-supplied). */
            snprintf(reply, sizeof(reply),
                     "{\"type\":\"set_time_result\",\"id\":\"%.64s\",\"ok\":%s,"
                     "\"epoch\":%lld,\"detail\":\"%.150s\"}",
                     id ? id : "", r.status == ESP_OK ? "true" : "false",
                     (long long)epoch, r.message);
            publish_reply(reply);
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

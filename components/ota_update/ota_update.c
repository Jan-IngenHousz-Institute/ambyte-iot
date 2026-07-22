#include "ota_update.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "ota_update"

#define OTA_TASK_STACK     8192
#define OTA_TASK_PRIO      4          /* below lua_runner(10); OTA is not latency-critical */
#define OTA_URL_MAX        256
#define OTA_ID_MAX         64
#define OTA_CONFIRM_TIMEOUT_S 300     /* wait this long for MQTT before rolling back a new image */
#define OTA_CONNECT_POLL_MS   1000

#define NVS_NS      "ota_upd"
#define KEY_APPLIED "applied_id"      /* id of the last successfully-applied image (set on success) */

typedef struct {
    char url[OTA_URL_MAX];
    char id[OTA_ID_MAX];
} ota_request_t;

static ota_update_config_t s_cfg;
static bool                s_ready;   /* init done; dispatch via the shared worker (s_cfg.submit) */

/* ── status reporting ──────────────────────────────────────────────────── */

/* Minimal JSON string escape — `id`/`detail` originate from the inbound command
 * (attacker-influenced via the command topic), so a stray quote/backslash/
 * control char would otherwise corrupt the status JSON. Truncates at cap. */
static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    if (in == NULL) { if (cap) out[0] = '\0'; return; }
    for (const char *p = in; *p != '\0' && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r')        { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t')        { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)         { /* drop other control chars */ }
        else                        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

static void ota_report(const char *state, const char *id, const char *detail)
{
    if (s_cfg.publish == NULL || s_cfg.status_topic == NULL || s_cfg.status_topic[0] == '\0') {
        return;
    }
    const esp_app_desc_t *d = esp_app_get_description();
    char esc_id[OTA_ID_MAX * 2 + 1] = "";
    char esc_detail[160] = "";
    json_escape(esc_id, sizeof esc_id, id);
    if (detail) json_escape(esc_detail, sizeof esc_detail, detail);

    char msg[384];
    int n = snprintf(msg, sizeof msg,
        "{\"type\":\"ota_status\",\"device_id\":\"%s\",\"id\":\"%s\",\"state\":\"%s\","
        "\"fw\":\"%.32s\"%s%s%s}",
        s_cfg.device_id ? s_cfg.device_id : "", esc_id, state,
        d ? d->version : "",
        detail ? ",\"detail\":\"" : "", esc_detail, detail ? "\"" : "");
    if (n <= 0 || (size_t)n >= sizeof msg) return;

    /* Retry briefly — a report just after reconnect can race a transient drop;
     * the publish no-ops while disconnected. Best-effort (status, not data). */
    for (int attempt = 0; attempt < 3; attempt++) {
        int msg_id = 0;
        if (s_cfg.publish(s_cfg.status_topic, msg, (size_t)n, &msg_id) == ESP_OK) return;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Poll until MQTT is connected or the timeout elapses (does not overshoot). */
static bool wait_connected(uint32_t timeout_s)
{
    if (s_cfg.is_connected == NULL) return false;
    uint32_t budget = timeout_s * 1000U;
    while (budget > 0) {
        if (s_cfg.is_connected()) return true;
        uint32_t step = (budget < OTA_CONNECT_POLL_MS) ? budget : OTA_CONNECT_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(step));
        budget -= step;
    }
    return s_cfg.is_connected();
}

/* ── applied-id latch (NVS) ─────────────────────────────────────────────
 * The id of the last image that was *successfully* written + booted. Set only
 * on success (right before reboot), so a FAILED download never burns the id —
 * re-sending the same id after a 404/network error just retries. Doubles as:
 * the dedupe key (a retained/duplicate trigger for an already-applied id is
 * ignored) and the correlation id the post-reboot confirm path reports. */

static void latch_set(const char *id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, KEY_APPLIED, id ? id : "");
    nvs_commit(h);
    nvs_close(h);
}

static bool latch_get(char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, KEY_APPLIED, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

/* True if `id` equals the last successfully-applied id. */
static bool already_applied(const char *id)
{
    if (id == NULL || id[0] == '\0') return false;
    char prev[OTA_ID_MAX] = "";
    return latch_get(prev, sizeof prev) && strcmp(prev, id) == 0;
}

/* ── one OTA download ──────────────────────────────────────────────────── */

static void ota_do_update(const ota_request_t *r)
{
    ESP_LOGW(TAG, "OTA requested id=%s url=%s", r->id, r->url);

    /* Guard the #1 foot-gun: a github.com *browse* URL (/blob/ or /tree/) serves
     * an HTML page, not the binary — esp_https_ota downloads it and rejects it as
     * a bad image ("Mismatch chip id … found 20569"). Catch it up front, while
     * MQTT is still up, with an actionable message. (/raw/ and release-asset
     * /releases/download/ URLs are fine and not matched here.) */
    if (strstr(r->url, "/blob/") != NULL || strstr(r->url, "/tree/") != NULL) {
        ESP_LOGE(TAG, "URL is a GitHub web page (/blob/ or /tree/), not a downloadable file:");
        ESP_LOGE(TAG, "  %s", r->url);
        ESP_LOGE(TAG, "use a RELEASE ASSET url — no /blob/ or /tree/, no branch name:");
        ESP_LOGE(TAG, "  https://github.com/<owner>/<repo>/releases/download/<tag>/firmware.bin");
        ESP_LOGE(TAG, "  (Releases page -> right-click the asset -> Copy link address)");
        ota_report("failed", r->id, "bad_url: use a release-asset link, not a /blob//tree/ web url");
        return;
    }

    ota_report("accepted", r->id, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the accepted report flush before comms drop */

    /* Quiesce the heap for the download (the board can't hold two TLS sessions):
     * stop the Lua measurement task (its 8 KB AMBIT buffer + transient tables
     * would fragment the heap mid-download), then free MQTT's TLS. */
    if (s_cfg.workload_suspend != NULL) s_cfg.workload_suspend();
    if (s_cfg.comms_suspend != NULL) s_cfg.comms_suspend();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Proven Stage-0 settings: cert bundle validates GitHub + its CDN across the
     * 302 redirect; 4 KiB HTTP buffers fit GitHub's long signed redirect URL. */
    esp_http_client_config_t http = {
        .url               = r->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    /* Advanced begin→perform→end API (not the one-shot esp_https_ota) so we can
     * yield once per chunk: on a fast link the one-shot loop runs CPU-bound and
     * starves a core's idle task past the 5 s task-WDT → panic. vTaskDelay(1)
     * lets idle run and feed the WDT; cost is negligible vs the download. */
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        do {
            err = esp_https_ota_perform(h);
            vTaskDelay(1);   /* yield so the idle task feeds the task-WDT */
        } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

        bool complete = (err == ESP_OK) && esp_https_ota_is_complete_data_received(h);
        if (err == ESP_OK && complete) {
            err = esp_https_ota_finish(h);   /* validates image + sets boot partition */
        } else {
            if (err == ESP_OK) err = ESP_FAIL;   /* incomplete download */
            esp_https_ota_abort(h);
        }
    }

    if (err == ESP_OK) {
        /* Mark applied ONLY now that the image is written + boot is set: a failed
         * download above never reaches here, so its id stays retryable. This
         * latch dedupes the retained trigger after reboot and is the id the
         * confirm path reports. */
        latch_set(r->id);
        ESP_LOGW(TAG, "OTA image written + boot set — rebooting into the new image");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();   /* no return */
    }

    /* Failed: not booting the new image — bring comms back and report. The id is
     * NOT latched, so the same id can be re-sent to retry. */
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    const char *detail = esp_err_to_name(err);
    if (err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_OTA_VALIDATE_FAILED) {
        /* Got bytes but the image header was invalid — almost always an HTML
         * page (wrong URL) rather than a firmware.bin. */
        ESP_LOGE(TAG, "downloaded content is not a valid ESP32-S3 image — did the URL "
                      "serve HTML? point at a release-asset firmware.bin");
        detail = "not_an_image: URL served non-firmware (HTML?) — use a release-asset .bin";
    }
    if (s_cfg.workload_resume != NULL) s_cfg.workload_resume();
    if (s_cfg.comms_resume != NULL) s_cfg.comms_resume();
    /* Match the confirm-path budget (300 s): a degraded link can take far longer
     * than 60 s to reconnect, and we'd otherwise drop the failure report. */
    if (wait_connected(OTA_CONFIRM_TIMEOUT_S)) {
        ota_report("failed", r->id, detail);
    }
}

/* ── post-reboot confirmation of a just-applied image ──────────────────── */

static void confirm_pending_after_boot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   /* normal boot — nothing to confirm */
    }

    char id[OTA_ID_MAX] = "";
    (void)latch_get(id, sizeof id);
    ESP_LOGW(TAG, "booted a PENDING_VERIFY image (id=%s) — confirming via MQTT", id);

    if (wait_connected(OTA_CONFIRM_TIMEOUT_S)) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            /* Could not commit the image as valid (flash/otadata error). Do NOT
             * report success — reboot so the bootloader cleanly handles the still-
             * PENDING_VERIFY image (it will roll back) rather than running on in an
             * unconfirmed state that a later reboot would silently revert. */
            ESP_LOGE(TAG, "mark_app_valid failed: %s — rebooting", esp_err_to_name(err));
            esp_restart();
        }
        ESP_LOGW(TAG, "image confirmed valid");
        /* Keep the latch as the applied-id dedupe record. */
        ota_report("success", id, NULL);
    } else {
        /* Couldn't prove health within the window — revert to the known-good slot.
         * The latch stays so the same (bad) id isn't auto-retried; a new id, or a
         * fixed image under a new id, is the way forward. */
        ESP_LOGE(TAG, "no MQTT within %ds — rolling back", OTA_CONFIRM_TIMEOUT_S);
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();   /* normally no return */
        ESP_LOGE(TAG, "rollback call returned (%s) — forcing reboot", esp_err_to_name(err));
        esp_restart();   /* never continue into the request loop while PENDING_VERIFY */
    }
}

/* Run one queued OTA op in the shared maintenance worker (fix #3). Owns and frees
 * `arg` (a heap ota_request_t). Reboots on success; returns on failure. */
static void ota_run(void *arg)
{
    ota_request_t *r = arg;
    /* Global maintenance gate: refuse to overlap another update type (a
     * concurrent AMBIT-OTA/script-update would fight for the heap and could hold
     * two TLS sessions → OOM). Redundant under the single shared worker, kept as
     * belt-and-suspenders. */
    if (s_cfg.maintenance_begin != NULL && !s_cfg.maintenance_begin()) {
        ESP_LOGW(TAG, "another maintenance op in progress — OTA id=%s dropped",
                 r->id[0] ? r->id : "");
        ota_report("dropped", r->id, "another maintenance op is in progress");
        free(r);
        return;
    }
    ota_do_update(r);   /* reboots on success; returns (and releases) on failure */
    if (s_cfg.maintenance_end != NULL) s_cfg.maintenance_end();
    free(r);
}

void ota_update_run_boot_confirm(void)
{
    confirm_pending_after_boot();   /* no-op fast return on a normal boot */
}

/* ── public API ────────────────────────────────────────────────────────── */

esp_err_t ota_update_init(const ota_update_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg   = *cfg;
    s_ready = true;   /* ops dispatch to the shared maintenance worker via s_cfg.submit */
    ESP_LOGI(TAG, "OTA module ready (shared maintenance worker)");
    return ESP_OK;
}

esp_err_t ota_update_request(const char *url, const char *id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (url == NULL || url[0] == '\0' || strlen(url) >= OTA_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Dedupe on the last successfully-applied id (idempotent under a retained
     * trigger). A failed attempt is NOT latched, so the same id retries. */
    if (already_applied(id)) {
        ESP_LOGI(TAG, "ota_update id=%s already applied — ignoring", id ? id : "");
        return ESP_OK;
    }
    /* Heap-copy the request and hand it to the shared worker. The copy is small
     * (~320 B), so it allocates even on a fragmented heap — unlike the old ~8 KB
     * task stack that this dispatch used to need. */
    ota_request_t *r = calloc(1, sizeof *r);
    if (r == NULL) {
        ota_report("dropped", id, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    strncpy(r->url, url, sizeof r->url - 1);
    if (id != NULL) strncpy(r->id, id, sizeof r->id - 1);
    if (s_cfg.submit == NULL || !s_cfg.submit(ota_run, r)) {
        /* Worker queue full — an update is already queued/in flight. */
        ota_report("dropped", id, "an OTA is already in progress");
        free(r);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

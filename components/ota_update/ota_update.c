#include "ota_update.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fleet_jitter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define TAG "ota_update"

#define OTA_TASK_STACK     8192
#define OTA_TASK_PRIO      4          /* below sched_runner(10); OTA is not latency-critical */
/* A Jobs delivery carries an S3 URL presigned under an assumed role, whose
 * X-Amz-Security-Token alone runs past 1 KB. The request is heap-sized to the
 * actual URL, so this cap costs nothing for the short command-path URLs; the
 * 4 KiB HTTP TX buffer below still fits a request line this long. */
#define OTA_URL_MAX        2048
/* AWS job ids run to 64 characters and the rollout workflow uses the full
 * width; the latch stores the id, so it needs the NUL on top. */
#define OTA_ID_MAX         72
#define OTA_SHA256_HEX     64
#define OTA_VERSION_MAX    40
#define OTA_VERIFY_CHUNK   4096
#define OTA_CONFIRM_TIMEOUT_S 300     /* wait this long for MQTT before rolling back a new image */
#define OTA_CONNECT_POLL_MS   1000
#define OTA_FLEET_JITTER_SLOTS 900U   /* one-second slots: 0:00 through 14:59 */
/* The reboot watchdog honours an admitted OTA for 30 minutes. Beyond that the
 * cheap admission latch is presumed wedged; the global maintenance lock still
 * protects an operation that is genuinely executing. */
#define OTA_WATCHDOG_VETO_MAX_US (30LL * 60 * 1000000)

_Static_assert(OTA_WATCHDOG_VETO_MAX_US >
               ((int64_t)(OTA_FLEET_JITTER_SLOTS - 1U) * 1000000LL),
               "OTA watchdog veto must cover the maximum fleet jitter");

#define NVS_NS      "ota_upd"
#define KEY_APPLIED "applied_id"      /* id of the last successfully-applied image (set on success) */

typedef struct {
    char id[OTA_ID_MAX];
    /* Job path only; empty on the command path, which checks neither. */
    char sha256[OTA_SHA256_HEX + 1];
    char expected_version[OTA_VERSION_MAX];
    bool fleet_spread;
    void (*started)(const char *id, void *ctx);
    void (*finished)(const char *id, bool retryable, const char *detail, void *ctx);
    void *cb_ctx;
    char url[];   /* sized to the actual URL at allocation */
} ota_request_t;

static ota_update_config_t s_cfg;
static bool                s_ready;   /* init done; dispatch via the shared worker (s_cfg.submit) */
/* Admission state is read from the watchdog task while commands mutate it on
 * MQTT/maintenance tasks. The timestamp is 64-bit on a 32-bit target, so keep
 * the complete tuple under one spinlock rather than relying on volatile. */
static bool                s_in_progress;
static bool                s_veto_expiry_warned;
static int64_t             s_admitted_at_us;
static portMUX_TYPE        s_progress_mux = portMUX_INITIALIZER_UNLOCKED;

static void ota_set_in_progress(bool active)
{
    portENTER_CRITICAL(&s_progress_mux);
    s_in_progress = active;
    s_admitted_at_us = active ? esp_timer_get_time() : 0;
    s_veto_expiry_warned = false;
    portEXIT_CRITICAL(&s_progress_mux);
}

static bool ota_is_admitted(void)
{
    portENTER_CRITICAL(&s_progress_mux);
    bool active = s_in_progress;
    portEXIT_CRITICAL(&s_progress_mux);
    return active;
}

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

/* Poll until SD/persistence reports healthy or the timeout elapses. Returns true
 * immediately when the check isn't wired (backward-compatible MQTT-only confirm).
 * Persistence comes up during boot init — well before MQTT connects — so a short
 * window is plenty; it only exists to absorb the maintenance-worker/init race. */
static bool wait_persistence_ok(uint32_t timeout_s)
{
    if (s_cfg.persistence_healthy == NULL) return true;
    uint32_t budget = timeout_s * 1000U;
    while (budget > 0) {
        if (s_cfg.persistence_healthy()) return true;
        uint32_t step = (budget < OTA_CONNECT_POLL_MS) ? budget : OTA_CONNECT_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(step));
        budget -= step;
    }
    return s_cfg.persistence_healthy();
}

/* Reserve the already-admitted maintenance worker while leaving MQTT and the
 * measurement workload running. A MAC-read failure deliberately falls back to
 * slot zero, matching the existing nightly-reboot policy. */
static void wait_for_fleet_slot(void)
{
    uint32_t delay_s = 0;
    esp_err_t err = fleet_jitter_slot_for_sta_mac(OTA_FLEET_JITTER_SLOTS, &delay_s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA MAC unavailable for fleet OTA jitter: %s — using 0 s",
                 esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "fleet OTA delay: %lu s", (unsigned long)delay_s);
    if (delay_s > 0U) {
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000U));
    }
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

/* ── job-path image checks ─────────────────────────────────────────────── */

/* The release tag reads `v1.3.0` while the build stamps `1.3.0`, so the one
 * tolerated difference is a leading v. Anything else, including a git-describe
 * build between releases, is a different version. */
static const char *strip_v(const char *s)
{
    return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
}

static bool hex_digit_value(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

/* SHA-256 of the first `len` bytes of `part`, read back from flash. Hashing
 * what landed on flash rather than the HTTP stream also catches a bad write,
 * and needs no hook into esp_https_ota's internals. Flash encryption is off on
 * this fleet, so the bytes read are the bytes the release published. */
static esp_err_t partition_sha256_matches(const esp_partition_t *part, size_t len,
                                          const char *expected_hex, bool *matches)
{
    uint8_t expected[32];
    for (size_t i = 0; i < sizeof expected; i++) {
        uint8_t hi, lo;
        if (!hex_digit_value(expected_hex[2 * i], &hi) ||
            !hex_digit_value(expected_hex[2 * i + 1], &lo)) {
            return ESP_ERR_INVALID_ARG;
        }
        expected[i] = (uint8_t)((hi << 4) | lo);
    }

    uint8_t *buf = malloc(OTA_VERIFY_CHUNK);
    if (buf == NULL) return ESP_ERR_NO_MEM;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    esp_err_t err = mbedtls_sha256_starts(&ctx, 0) == 0 ? ESP_OK : ESP_FAIL;
    for (size_t off = 0; err == ESP_OK && off < len; off += OTA_VERIFY_CHUNK) {
        size_t n = len - off < OTA_VERIFY_CHUNK ? len - off : OTA_VERIFY_CHUNK;
        err = esp_partition_read(part, off, buf, n);
        if (err == ESP_OK && mbedtls_sha256_update(&ctx, buf, n) != 0) err = ESP_FAIL;
        vTaskDelay(1);   /* ~3 MB of reads; let the idle task feed the WDT */
    }
    uint8_t actual[32];
    if (err == ESP_OK && mbedtls_sha256_finish(&ctx, actual) != 0) err = ESP_FAIL;
    mbedtls_sha256_free(&ctx);
    free(buf);

    if (err == ESP_OK) *matches = memcmp(actual, expected, sizeof actual) == 0;
    return err;
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
        if (r->finished != NULL) {
            r->finished(r->id, false, "bad url: a GitHub web page, not a file", r->cb_ctx);
        }
        return;
    }

    ota_report("accepted", r->id, NULL);
    if (r->started != NULL) r->started(r->id, r->cb_ctx);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the accepted report flush before comms drop */

    /* The command path is a fleet-wide fan-out with no pacing of its own. A job
     * already arrives paced by AWS's per-minute rollout cap, and its presigned
     * URL expires within the hour, so the 15-minute spread would only eat into
     * that window. */
    if (r->fleet_spread) wait_for_fleet_slot();

    /* Quiesce the heap for the download (the board can't hold two TLS sessions):
     * stop the schedule runner (its transient action allocations
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
    const char *check_detail = NULL;   /* set when a job-path identity check refuses the image */
    char check_buf[128];
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK && r->expected_version[0] != '\0') {
        /* Reads just the image header: a wrong asset is refused before ~3 MB
         * of download, not after. */
        esp_app_desc_t incoming = {0};
        err = esp_https_ota_get_img_desc(h, &incoming);
        if (err == ESP_OK &&
            strcmp(strip_v(incoming.version), strip_v(r->expected_version)) != 0) {
            snprintf(check_buf, sizeof check_buf, "image is %.32s, job expects %.32s",
                     incoming.version, r->expected_version);
            check_detail = check_buf;
            err = ESP_ERR_INVALID_VERSION;
        }
        if (err != ESP_OK) esp_https_ota_abort(h);
    }
    if (err == ESP_OK) {
        do {
            err = esp_https_ota_perform(h);
            vTaskDelay(1);   /* yield so the idle task feeds the task-WDT */
        } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

        bool complete = (err == ESP_OK) && esp_https_ota_is_complete_data_received(h);
        if (err == ESP_OK && complete && r->sha256[0] != '\0') {
            /* Before finish(), which is what switches the boot partition: a
             * digest mismatch must leave the running image selected. */
            const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
            int len = esp_https_ota_get_image_len_read(h);
            bool matches = false;
            err = (target == NULL || len <= 0)
                ? ESP_FAIL
                : partition_sha256_matches(target, (size_t)len, r->sha256, &matches);
            if (err == ESP_OK && !matches) {
                check_detail = "sha256 mismatch: downloaded image is not the released one";
                err = ESP_ERR_OTA_VALIDATE_FAILED;
            } else if (err != ESP_OK) {
                check_detail = "could not read back the image to verify sha256";
            }
            if (err != ESP_OK) complete = false;
        }
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
    if (check_detail != NULL) {
        detail = check_detail;
    } else if (err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_OTA_VALIDATE_FAILED) {
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
    /* Called even if the reconnect window passed: the publish no-ops while
     * offline, and a job left IN_PROGRESS is retried on the next $next/get
     * (not latched) or timed out by AWS, never silently dropped. */
    if (r->finished != NULL) r->finished(r->id, false, detail, r->cb_ctx);
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

    /* Health = MQTT reconnect AND SD/persistence up. The persistence check catches
     * an image that comes online but breaks SD mounting / the event log — which the
     * MQTT-only gate would have kept, stranding the unit measurement-dead. */
    if (wait_connected(OTA_CONFIRM_TIMEOUT_S) && wait_persistence_ok(60)) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            /* Could not commit the image as valid (flash/otadata error). Do NOT
             * report success — reboot so the bootloader cleanly handles the still-
             * PENDING_VERIFY image (it will roll back) rather than running on in an
             * unconfirmed state that a later reboot would silently revert. */
            ESP_LOGE(TAG, "mark_app_valid failed: %s — rebooting", esp_err_to_name(err));
            esp_restart();
        }
        ESP_LOGW(TAG, "image confirmed valid (MQTT + persistence healthy)");
        /* Keep the latch as the applied-id dedupe record. */
        ota_report("success", id, NULL);
        /* A job execution for this image has been parked IN_PROGRESS since
         * before the reboot; only now may it be reported SUCCEEDED. */
        if (s_cfg.confirmed != NULL) s_cfg.confirmed();
    } else {
        /* Couldn't prove health within the window — revert to the known-good slot.
         * The latch stays so the same (bad) id isn't auto-retried; a new id, or a
         * fixed image under a new id, is the way forward. */
        ESP_LOGE(TAG, "health gate failed (MQTT/persistence) within %ds — rolling back",
                 OTA_CONFIRM_TIMEOUT_S);
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
        ota_set_in_progress(false);
        if (r->finished != NULL) {
            r->finished(r->id, true, "another maintenance op is in progress", r->cb_ctx);
        }
        free(r);
        return;
    }
    ota_do_update(r);   /* reboots on success; returns (and releases) on failure */
    if (s_cfg.maintenance_end != NULL) s_cfg.maintenance_end();
    ota_set_in_progress(false);
    free(r);
}

void ota_update_run_boot_confirm(void)
{
    confirm_pending_after_boot();   /* no-op fast return on a normal boot */
}

/* Heap-copy a request, sized to its URL, for the shared worker. Even with a
 * presigned URL it stays ~2 KB, so it allocates on a fragmented heap — unlike
 * the old ~8 KB task stack this dispatch used to need. */
static ota_request_t *request_alloc(const char *url, const char *id)
{
    size_t url_len = strlen(url);
    ota_request_t *r = calloc(1, sizeof *r + url_len + 1);
    if (r == NULL) return NULL;
    memcpy(r->url, url, url_len + 1);
    if (id != NULL) strncpy(r->id, id, sizeof r->id - 1);
    return r;
}

/* Takes ownership of `r`. On failure nothing is queued and no callback fires,
 * which is why the job path learns about a full queue from the return value. */
static esp_err_t request_submit(ota_request_t *r)
{
    if (ota_is_admitted()) {
        ota_report("dropped", r->id, "an OTA is already in progress");
        free(r);
        return ESP_ERR_INVALID_STATE;
    }
    /* Latch before queue admission so the watchdog cannot race the interval
     * between accepting a remote command and the shared worker starting it. */
    ota_set_in_progress(true);
    if (s_cfg.submit == NULL || !s_cfg.submit(ota_run, r)) {
        /* Worker queue full — an update is already queued/in flight. */
        ota_report("dropped", r->id, "an OTA is already in progress");
        ota_set_in_progress(false);
        free(r);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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
    ota_request_t *r = request_alloc(url, id);
    if (r == NULL) {
        ota_report("dropped", id, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    r->fleet_spread = true;
    return request_submit(r);
}

esp_err_t ota_update_request_job(const ota_update_job_t *job)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (job == NULL || job->url == NULL || job->url[0] == '\0' ||
        strlen(job->url) >= OTA_URL_MAX || job->id == NULL || job->id[0] == '\0' ||
        strlen(job->id) >= OTA_ID_MAX || job->sha256 == NULL ||
        strlen(job->sha256) != OTA_SHA256_HEX || job->expected_version == NULL ||
        job->expected_version[0] == '\0' ||
        strlen(job->expected_version) >= OTA_VERSION_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_request_t *r = request_alloc(job->url, job->id);
    if (r == NULL) return ESP_ERR_NO_MEM;
    memcpy(r->sha256, job->sha256, OTA_SHA256_HEX + 1);
    strncpy(r->expected_version, job->expected_version, sizeof r->expected_version - 1);
    r->started  = job->started;
    r->finished = job->finished;
    r->cb_ctx   = job->ctx;
    return request_submit(r);
}

bool ota_update_applied_id_is(const char *id)
{
    return already_applied(id);
}

bool ota_update_in_progress(void)
{
    bool active;
    bool warn = false;
    int64_t age_us = 0;

    portENTER_CRITICAL(&s_progress_mux);
    active = s_in_progress;
    if (active) {
        age_us = esp_timer_get_time() - s_admitted_at_us;
        if (age_us >= OTA_WATCHDOG_VETO_MAX_US) {
            active = false;  /* expire only the self-healing veto, not admission */
            if (!s_veto_expiry_warned) {
                s_veto_expiry_warned = true;
                warn = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_progress_mux);

    if (warn) {
        ESP_LOGW(TAG, "OTA watchdog veto expired after %lld s; self-healing resumes",
                 (long long)(age_us / 1000000LL));
    }
    return active;
}

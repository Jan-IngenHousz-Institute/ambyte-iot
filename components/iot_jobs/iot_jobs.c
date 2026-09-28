#include "iot_jobs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_jobs_plan.h"
#include "ota_update.h"

#define TAG "iot_jobs"

#define JOBS_TOPIC_MAX   192    /* the transport's inbound topic cap */
/* "$aws/things/" + a 128-char Thing name (AWS's max) + "/jobs" is 145; with
 * the longest suffix below ("/$next/get/+", 12) every topic fits 192. */
#define JOBS_BASE_MAX    (JOBS_TOPIC_MAX - 32)
#define JOBS_ID_MAX      72     /* AWS job ids are <= 64 chars */
#define JOBS_REPORT_MAX  512
/* A retryable miss (the maintenance worker was busy with another update type)
 * leaves the execution QUEUED; look again after this long rather than waiting
 * for the next reconnect, which on a healthy link may be days away. */
#define JOBS_RETRY_US    (10LL * 60 * 1000000)
/* If the grants never come back, the FAILED report is lost and the next
 * $next/get retries the install; AWS's in-progress timeout still bounds it. */
#define JOBS_AUTH_WAIT_MS 30000

enum { SUB_NOTIFY, SUB_GET, SUB_COUNT };

static iot_jobs_config_t s_cfg;
static bool              s_ready;
static char              s_topic_notify[JOBS_TOPIC_MAX];     /* .../jobs/notify-next */
static char              s_topic_get_filter[JOBS_TOPIC_MAX]; /* .../jobs/$next/get/+ */
static char              s_topic_get[JOBS_TOPIC_MAX];        /* .../jobs/$next/get */
static char              s_topic_jobs[JOBS_BASE_MAX];        /* .../jobs, for <jobId>/update */
static esp_timer_handle_t s_retry_timer;

/* Touched from the esp-mqtt task (SUBACKs, messages), the maintenance worker
 * (OTA callbacks) and the esp_timer task (retry), so guarded; every critical
 * section is a copy, never a publish. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_granted_session[SUB_COUNT];   /* 0 = not granted this boot */
static bool         s_refusal_logged;
static char         s_active_job[JOBS_ID_MAX];      /* handed to the OTA worker */

/* The session the transport is on right now, or 0 while disconnected. */
static uint32_t live_session(void)
{
    if (s_cfg.connection_stats == NULL) return 0;
    uint32_t connects = 0;
    int64_t age_s = -1;
    s_cfg.connection_stats(&connects, &age_s, NULL, 0);
    return age_s >= 0 ? connects : 0;
}

/* Both job subscriptions granted on the live connection: the only state in
 * which a publish on a job topic is known to be inside the policy. */
static bool authorized(void)
{
    uint32_t session = live_session();
    if (session == 0) return false;
    portENTER_CRITICAL(&s_mux);
    bool ok = s_granted_session[SUB_NOTIFY] == session &&
              s_granted_session[SUB_GET] == session;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void set_active(const char *job_id)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_active_job, job_id != NULL ? job_id : "", sizeof s_active_job - 1);
    s_active_job[sizeof s_active_job - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

static bool is_active(const char *job_id)
{
    portENTER_CRITICAL(&s_mux);
    bool active = s_active_job[0] != '\0' && strcmp(s_active_job, job_id) == 0;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (const char *p = in != NULL ? in : ""; *p != '\0' && o + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c >= 0x20)        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

/* UpdateJobExecution. statusDetails values must be strings. No expectedVersion:
 * this device is the execution's only writer, so optimistic locking would only
 * add a rejection path with nothing to recover. */
static void report(const char *job_id, const char *status, const char *reason)
{
    if (!authorized()) {
        ESP_LOGW(TAG, "not reporting %s for %s: job topics not granted on this connection",
                 status, job_id);
        return;
    }
    char topic[JOBS_TOPIC_MAX + JOBS_ID_MAX + 8];
    snprintf(topic, sizeof topic, "%s/%s/update", s_topic_jobs, job_id);

    char esc_reason[160], esc_version[48];
    json_escape(esc_reason, sizeof esc_reason, reason);
    json_escape(esc_version, sizeof esc_version, s_cfg.running_version);
    char body[JOBS_REPORT_MAX];
    int n = snprintf(body, sizeof body,
                     "{\"status\":\"%s\",\"statusDetails\":"
                     "{\"reason\":\"%s\",\"runningVersion\":\"%s\"}}",
                     status, esc_reason, esc_version);
    if (n <= 0 || (size_t)n >= sizeof body) return;

    esp_err_t err = s_cfg.publish(topic, body, (size_t)n, NULL);
    ESP_LOGW(TAG, "job %s -> %s (%s)%s", job_id, status, reason != NULL ? reason : "",
             err == ESP_OK ? "" : " [publish failed]");
}

static void query_next(void)
{
    if (!authorized()) return;
    static const char body[] = "{}";
    esp_err_t err = s_cfg.publish(s_topic_get, body, sizeof body - 1, NULL);
    ESP_LOGI(TAG, "asked for the next job execution (%s)", esp_err_to_name(err));
}

static void schedule_retry(void)
{
    if (s_retry_timer == NULL) return;
    esp_timer_stop(s_retry_timer);   /* restart the window; ESP_ERR_INVALID_STATE if idle */
    esp_timer_start_once(s_retry_timer, JOBS_RETRY_US);
}

static void retry_cb(void *arg)
{
    (void)arg;
    query_next();
}

/* ── OTA worker callbacks (maintenance task) ─────────────────────────────── */

static void on_ota_started(const char *id, void *ctx)
{
    (void)ctx;
    report(id, "IN_PROGRESS", "downloading");
}

static void on_ota_finished(const char *id, bool retryable, const char *detail, void *ctx)
{
    (void)ctx;
    if (retryable) {
        set_active(NULL);
        ESP_LOGW(TAG, "job %s not started (%s); retrying later", id, detail);
        schedule_retry();
        return;
    }
    /* A failed download resumes MQTT, and the worker calls back as soon as it
     * reconnects, typically before the job SUBACKs of that connection return.
     * Wait for them (this is the maintenance worker; blocking is fine), and keep
     * the job marked active meanwhile: the SUBACK-triggered $next/get answers
     * with this same IN_PROGRESS execution, and without the mark it would be
     * installed again instead of reported FAILED. */
    for (int waited_ms = 0; !authorized() && waited_ms < JOBS_AUTH_WAIT_MS;
         waited_ms += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    report(id, "FAILED", detail);
    set_active(NULL);
}

/* ── inbound (esp-mqtt task) ─────────────────────────────────────────────── */

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void handle_execution(const cJSON *execution)
{
    const char *job_id = json_str(execution, "jobId");
    const char *status = json_str(execution, "status");
    const cJSON *jdoc = cJSON_GetObjectItemCaseSensitive(execution, "jobDocument");
    if (job_id == NULL || strlen(job_id) >= JOBS_ID_MAX) {
        ESP_LOGW(TAG, "execution without a usable jobId; ignoring");
        return;
    }
    /* notify-next repeats while our own update is under way (every status
     * change of the queue). The worker owns it until it reboots or calls back. */
    if (is_active(job_id)) {
        ESP_LOGI(TAG, "job %s already being installed", job_id);
        return;
    }

    iot_jobs_document_t doc = {
        .operation = cJSON_IsObject(jdoc) ? json_str(jdoc, "operation") : NULL,
        .family    = cJSON_IsObject(jdoc) ? json_str(jdoc, "family")    : NULL,
        .version   = cJSON_IsObject(jdoc) ? json_str(jdoc, "version")   : NULL,
        .sha256    = cJSON_IsObject(jdoc) ? json_str(jdoc, "sha256")    : NULL,
        .url       = cJSON_IsObject(jdoc) ? json_str(jdoc, "url")       : NULL,
    };
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t *running = esp_ota_get_running_partition();
    bool pending = running != NULL &&
                   esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    iot_jobs_device_t dev = {
        .family           = s_cfg.family,
        .running_version  = s_cfg.running_version,
        .pending_verify   = pending,
        .applied_this_job = ota_update_applied_id_is(job_id),
    };

    const char *reason = "";
    iot_jobs_action_t action = iot_jobs_plan(&doc, &dev, &reason);
    ESP_LOGW(TAG, "job %s (%s, version %s): %s", job_id, status != NULL ? status : "?",
             doc.version != NULL ? doc.version : "?", reason);

    switch (action) {
    case IOT_JOBS_REJECT:  report(job_id, "REJECTED", reason);  break;
    case IOT_JOBS_SUCCEED: report(job_id, "SUCCEEDED", reason); break;
    case IOT_JOBS_FAIL:    report(job_id, "FAILED", reason);    break;
    case IOT_JOBS_DEFER:   break;   /* ota confirm calls iot_jobs_kick */
    case IOT_JOBS_INSTALL: {
        set_active(job_id);
        ota_update_job_t job = {
            .url              = doc.url,
            .id               = job_id,
            .sha256           = doc.sha256,
            .expected_version = doc.version,
            .started          = on_ota_started,
            .finished         = on_ota_finished,
        };
        esp_err_t err = ota_update_request_job(&job);
        if (err == ESP_ERR_INVALID_ARG) {
            set_active(NULL);
            report(job_id, "REJECTED", "job document exceeds device limits");
        } else if (err != ESP_OK) {
            /* Another update is admitted, or no heap. Nothing reported, so the
             * execution stays QUEUED for the retry. */
            set_active(NULL);
            ESP_LOGW(TAG, "job %s not admitted (%s); retrying later", job_id,
                     esp_err_to_name(err));
            schedule_retry();
        }
        break;
    }
    }
}

static void on_jobs_message(const char *topic, const char *payload, size_t len, void *ctx)
{
    (void)ctx;
    size_t topic_len = strlen(topic);
    if (topic_len >= 9 && strcmp(topic + topic_len - 9, "/rejected") == 0) {
        ESP_LOGW(TAG, "jobs request rejected: %.*s", (int)(len < 200 ? len : 200), payload);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "unparseable jobs message on %s", topic);
        return;
    }
    const cJSON *execution = cJSON_GetObjectItemCaseSensitive(root, "execution");
    if (cJSON_IsObject(execution)) {
        handle_execution(execution);
    } else {
        ESP_LOGI(TAG, "no pending job execution");
    }
    cJSON_Delete(root);
}

static void on_suback(bool granted, uint32_t session, void *ctx)
{
    int which = (int)(intptr_t)ctx;
    bool both = false;
    bool log_refusal = false;
    portENTER_CRITICAL(&s_mux);
    s_granted_session[which] = granted ? session : 0;
    both = s_granted_session[SUB_NOTIFY] == session && s_granted_session[SUB_GET] == session;
    if (!granted && !s_refusal_logged) {
        s_refusal_logged = true;
        log_refusal = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (log_refusal) {
        ESP_LOGW(TAG, "broker refused the job topics for thing '%s': the certificate has no "
                      "Jobs policy, or the client id is not the Thing name. Firmware jobs "
                      "stay off; nothing is published on job topics.", s_cfg.thing_name);
    }
    /* The second grant of a connection is the moment to ask: covers boot, every
     * reconnect, and a job queued while the device was offline. */
    if (both) query_next();
}

/* ── public API ──────────────────────────────────────────────────────────── */

esp_err_t iot_jobs_init(const iot_jobs_config_t *cfg)
{
    if (cfg == NULL || cfg->thing_name == NULL || cfg->thing_name[0] == '\0' ||
        cfg->family == NULL || cfg->running_version == NULL || cfg->publish == NULL ||
        cfg->add_subscription == NULL || cfg->connection_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;

    int n = snprintf(s_topic_jobs, sizeof s_topic_jobs, "$aws/things/%s/jobs", cfg->thing_name);
    if (n <= 0 || (size_t)n >= sizeof s_topic_jobs) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(s_topic_notify, sizeof s_topic_notify, "%s/notify-next", s_topic_jobs);
    snprintf(s_topic_get, sizeof s_topic_get, "%s/$next/get", s_topic_jobs);
    snprintf(s_topic_get_filter, sizeof s_topic_get_filter, "%s/$next/get/+", s_topic_jobs);

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "jobs_retry" };
    esp_err_t err = esp_timer_create(&targs, &s_retry_timer);
    if (err != ESP_OK) return err;

    err = cfg->add_subscription(s_topic_notify, on_jobs_message, on_suback,
                                (void *)(intptr_t)SUB_NOTIFY);
    if (err == ESP_OK) {
        err = cfg->add_subscription(s_topic_get_filter, on_jobs_message, on_suback,
                                    (void *)(intptr_t)SUB_GET);
    }
    if (err != ESP_OK) return err;

    s_ready = true;
    ESP_LOGI(TAG, "listening for firmware jobs as thing '%s'", cfg->thing_name);
    return ESP_OK;
}

void iot_jobs_kick(void)
{
    if (s_ready) query_next();
}

/*
 * location_aws.c — schedules and runs AWS IoT Core Device Location requests
 * (contract, gating and cadence in location_aws.h). Payload encoding/decoding
 * lives in location_aws_codec.c.
 *
 * Tasks: SUBACKs and replies arrive on the esp-mqtt task, which only copies
 * the reply into a slot and wakes the waiter (no NVS, no event_log). The
 * attempt itself -- Wi-Fi scan, plain-HTTP public-IP lookup, publish, wait --
 * runs on the resident maintenance worker under the maintenance gate (a scan
 * during an OTA download would stall it). The accepted estimate goes to
 * timezone_command_offer_live(), i.e. the same worker path as every other
 * timezone input. The esp_timer callback only submits.
 */
#include "location_aws.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "device_config.h"
#include "device_location.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "timezone_command.h"

#define TAG "loc_aws"

#define TOPIC_MAX        (LOCATION_AWS_ID_MAX + 64)
#define FIRST_MIN_S      60
#define FIRST_SPAN_S     540          /* first attempt 1..10 min after the grant */
#define INTERVAL_S       (6 * 3600)
#define MAX_INTERVAL_S   (24 * 3600)
#define BUSY_RETRY_S     (15 * 60)
#define REPLY_WAIT_MS    5000

enum { SUB_ACCEPTED, SUB_REJECTED, SUB_COUNT };

static location_aws_config_t s_cfg;
static char s_loc_id[LOCATION_AWS_ID_MAX];
static char s_topic_request[TOPIC_MAX];
static char s_topic_accepted[TOPIC_MAX + 16];
static char s_topic_rejected[TOPIC_MAX + 16];
static esp_timer_handle_t s_timer;
static SemaphoreHandle_t s_reply_sem;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_granted_session[SUB_COUNT];
static bool s_refusal_logged;
static bool s_scheduled_this_boot;
static bool s_job_pending;
static char s_waiting_id[48];          /* MessageId the worker waits for */
static location_aws_reply_t s_reply;   /* filled by the esp-mqtt task */
static bool s_reply_ready;
static uint32_t s_interval_s = INTERVAL_S;
static uint32_t s_sequence;
static bool s_force_ip;                /* next attempt skips Wi-Fi (locate ip) */

static uint32_t live_session(void)
{
    if (s_cfg.connection_stats == NULL) return 0;
    uint32_t connects = 0;
    int64_t age_s = -1;
    s_cfg.connection_stats(&connects, &age_s, NULL, 0);
    return age_s >= 0 ? connects : 0;
}

/* Both reply subscriptions granted on the live connection: the only state in
 * which a publish on the request topic is known to be inside the policy. */
static bool authorized(void)
{
    uint32_t session = live_session();
    if (session == 0) return false;
    portENTER_CRITICAL(&s_mux);
    bool ok = s_granted_session[SUB_ACCEPTED] == session && s_granted_session[SUB_REJECTED] == session;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static bool enabled(void)
{
    char mode[8];
    /* Absent = "aws" (default on; inert without the policy anyway). */
    return device_config_get_live_location_mode(mode, sizeof mode) != ESP_OK ||
           strcmp(mode, "off") != 0;
}

static void arm(uint32_t seconds)
{
    if (s_timer == NULL) return;
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, (uint64_t)seconds * 1000000ULL);
}

/* interval +-10 % so a fleet recovering from a broker outage spreads out. */
static uint32_t jittered(uint32_t seconds)
{
    uint32_t spread = seconds / 5;
    return seconds - seconds / 10 + (spread ? esp_random() % spread : 0);
}

static void status(const char *text)
{
    device_location_set_live_status(text);
}

/* Publish one request and wait for its reply (worker task). */
static bool request(const char *body, size_t len, const char *message_id, location_aws_reply_t *reply)
{
    if (!authorized()) return false;
    portENTER_CRITICAL(&s_mux);
    snprintf(s_waiting_id, sizeof s_waiting_id, "%s", message_id);
    s_reply_ready = false;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreTake(s_reply_sem, 0);   /* drop a stale wake */
    if (s_cfg.publish(s_topic_request, body, len, NULL) != ESP_OK) return false;
    bool got = xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(REPLY_WAIT_MS)) == pdTRUE;
    portENTER_CRITICAL(&s_mux);
    got = got && s_reply_ready;
    if (got) *reply = s_reply;
    s_waiting_id[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
    return got;
}

static void attempt(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    s_job_pending = false;
    portEXIT_CRITICAL(&s_mux);
    if (!enabled()) {
        status("off");
        arm(jittered(INTERVAL_S));
        return;
    }
    if (!authorized()) {
        /* Not granted on this connection (offline, or the policy is gone).
         * Nothing is armed now: the next grant of both reply subscriptions
         * re-arms the schedule (on_suback), so a timer that fired while
         * offline cannot end live location for the rest of the boot. */
        status("not_authorized");
        return;
    }
    if (s_cfg.maintenance_begin && !s_cfg.maintenance_begin()) {
        status("busy");
        arm(BUSY_RETRY_S);
        return;
    }
    int64_t now = (int64_t)time(NULL);
    char id[40], body[LOCATION_AWS_REQUEST_MAX];
    location_aws_reply_t reply = {0};
    bool have = false;
    const char *how = "none";

    portENTER_CRITICAL(&s_mux);
    bool ip_only = s_force_ip;
    s_force_ip = false;
    portEXIT_CRITICAL(&s_mux);
    location_aws_ap_t raw[20], aps[LOCATION_AWS_MAX_APS];
    int n_raw = (!ip_only && s_cfg.wifi_scan) ? s_cfg.wifi_scan(raw, 20) : 0;
    int n = location_aws_select_aps(raw, n_raw < 0 ? 0 : n_raw, aps, LOCATION_AWS_MAX_APS);
    if (n >= 2) {
        snprintf(id, sizeof id, "tz-%" PRIu32 "-w", ++s_sequence);
        size_t len = location_aws_wifi_request(id, now, aps, n, body, sizeof body);
        have = len && request(body, len, id, &reply);
        how = "wifi";
    }
    /* Unknown APs answer 404 (never an IP fallback), so retry with the IP
     * form; also when too few APs are visible to try Wi-Fi at all. */
    if (!have || (!reply.accepted && reply.error_code == 404)) {
        char text[64] = "", ip[48];
        if (s_cfg.public_ip && s_cfg.public_ip(text, sizeof text) &&
            location_aws_public_ip(text, ip, sizeof ip)) {
            snprintf(id, sizeof id, "tz-%" PRIu32 "-i", ++s_sequence);
            size_t len = location_aws_ip_request(id, now, ip, body, sizeof body);
            memset(&reply, 0, sizeof reply);
            have = len && request(body, len, id, &reply);
            how = "ip";
        } else if (!have) {
            how = "no_public_ip";
        }
    }
    if (s_cfg.maintenance_end) s_cfg.maintenance_end();

    if (have && reply.accepted) {
        ESP_LOGW(TAG, "estimate (%s): %.4f,%.4f +-%.0f m", how, reply.lat, reply.lon, reply.accuracy_m);
        s_interval_s = INTERVAL_S;
        timezone_command_offer_live(reply.lat, reply.lon, reply.accuracy_m, now,
                                    strcmp(how, "wifi") == 0 ? DEVICE_LIVE_SOURCE_WIFI
                                                             : DEVICE_LIVE_SOURCE_IP);
    } else {
        char text[24];
        if (have) snprintf(text, sizeof text, "rejected_%d", reply.error_code);
        else snprintf(text, sizeof text, "%s", strcmp(how, "no_public_ip") ? "no_reply" : how);
        status(text);
        ESP_LOGW(TAG, "no estimate (%s, %s); next in %" PRIu32 " h", how, text,
                 (uint32_t)(s_interval_s * 2 > MAX_INTERVAL_S ? MAX_INTERVAL_S : s_interval_s * 2) / 3600);
        s_interval_s = s_interval_s * 2 > MAX_INTERVAL_S ? MAX_INTERVAL_S : s_interval_s * 2;
    }
    arm(jittered(s_interval_s));
}

static void submit_attempt(void)
{
    portENTER_CRITICAL(&s_mux);
    bool already = s_job_pending;
    s_job_pending = true;
    portEXIT_CRITICAL(&s_mux);
    if (already) return;
    if (s_cfg.submit == NULL || !s_cfg.submit(attempt, NULL)) {
        portENTER_CRITICAL(&s_mux);
        s_job_pending = false;
        portEXIT_CRITICAL(&s_mux);
        arm(BUSY_RETRY_S);
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    submit_attempt();
}

/* esp-mqtt task: copy, match, wake. */
static void on_reply(const char *topic, const char *payload, size_t len, void *ctx)
{
    (void)ctx;
    bool rejected = strcmp(topic, s_topic_rejected) == 0;
    location_aws_reply_t reply;
    if (!location_aws_parse_reply(rejected, payload, len, &reply)) {
        ESP_LOGW(TAG, "unparseable device-location reply on %s", topic);
        return;
    }
    bool wake = false;
    portENTER_CRITICAL(&s_mux);
    if (s_waiting_id[0] && strcmp(s_waiting_id, reply.message_id) == 0) {
        s_reply = reply;
        s_reply_ready = true;
        wake = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (wake) xSemaphoreGive(s_reply_sem);
}

static void on_suback(bool granted, uint32_t session, void *ctx)
{
    int which = (int)(intptr_t)ctx;
    bool both, first = false, log_refusal = false;
    portENTER_CRITICAL(&s_mux);
    s_granted_session[which] = granted ? session : 0;
    both = s_granted_session[SUB_ACCEPTED] == session && s_granted_session[SUB_REJECTED] == session;
    if (both && !s_scheduled_this_boot) {
        s_scheduled_this_boot = true;
        first = true;
    }
    bool pending = s_job_pending;
    portEXIT_CRITICAL(&s_mux);
    /* Any later grant re-arms only if nothing is scheduled (the timer fired
     * while unauthorized, gpt-6-astra/Opus review of #100): a healthy pending
     * 6 h timer is left alone, so reconnect churn adds no requests. */
    bool rearm = both && !first && !pending && s_timer != NULL && !esp_timer_is_active(s_timer);
    portENTER_CRITICAL(&s_mux);
    if (!granted && !s_refusal_logged) {
        s_refusal_logged = true;
        log_refusal = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (log_refusal) {
        ESP_LOGW(TAG, "broker refused the device-location topics for '%s': no policy on this "
                      "certificate, or no deviceLocationId attribute. Live location stays off; "
                      "nothing is published.", s_loc_id);
        device_location_set_live_status("not_authorized");
    }
    /* First grant of the boot: one attempt shortly after, jittered so a fleet
     * reconnecting together does not hit the account quota at once. Later
     * reconnects do not add requests; the 6 h timer carries on. */
    if (first || rearm) arm(FIRST_MIN_S + esp_random() % FIRST_SPAN_S);
}

esp_err_t location_aws_init(const location_aws_config_t *cfg)
{
    if (cfg == NULL || cfg->publish == NULL || cfg->add_subscription == NULL ||
        cfg->connection_stats == NULL || cfg->submit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    if (!location_aws_loc_id(cfg->thing_name, s_loc_id, sizeof s_loc_id)) return ESP_ERR_INVALID_ARG;
    snprintf(s_topic_request, sizeof s_topic_request,
             "$aws/device_location/%s/get_position_estimate", s_loc_id);
    snprintf(s_topic_accepted, sizeof s_topic_accepted, "%s/accepted", s_topic_request);
    snprintf(s_topic_rejected, sizeof s_topic_rejected, "%s/rejected", s_topic_request);
    s_reply_sem = xSemaphoreCreateBinary();
    if (s_reply_sem == NULL) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t targs = { .callback = timer_cb, .name = "loc_aws" };
    esp_err_t err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) return err;
    err = cfg->add_subscription(s_topic_accepted, on_reply, on_suback, (void *)(intptr_t)SUB_ACCEPTED);
    if (err == ESP_OK) {
        err = cfg->add_subscription(s_topic_rejected, on_reply, on_suback, (void *)(intptr_t)SUB_REJECTED);
    }
    if (err != ESP_OK) return err;
    device_location_set_live_status(enabled() ? "waiting" : "off");
    ESP_LOGI(TAG, "device location via AWS as '%s' (waits for the policy grant)", s_loc_id);
    return ESP_OK;
}

const char *location_aws_request_now(bool ip_only)
{
    if (s_timer == NULL) return "not_started";
    if (!authorized()) return "not_authorized";
    portENTER_CRITICAL(&s_mux);
    s_force_ip = ip_only;
    portEXIT_CRITICAL(&s_mux);
    submit_attempt();
    return "queued";
}

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_manager.h"

/* The manager's own result must never be mistaken for a driver code: in
 * particular esp_wifi_set_config() can return ESP_ERR_WIFI_PASSWORD for a
 * malformed password BEFORE anything is saved or attempted, and the CLI's
 * "saved, AP rejected, still retrying" hint would then be false. */
_Static_assert((WIFI_MANAGER_ERR_AUTH_REJECTED < ESP_ERR_WIFI_BASE) ||
               (WIFI_MANAGER_ERR_AUTH_REJECTED >= ESP_ERR_MESH_BASE),
               "WIFI_MANAGER_ERR_* must stay outside the esp_wifi error range");

#define WIFI_MANAGER_CONNECTED_BIT BIT0
#define WIFI_MANAGER_FAILED_BIT BIT1
#define WIFI_MANAGER_INITIAL_CONNECT_TIMEOUT_MS 10000
#define WIFI_MANAGER_STA_IFKEY "WIFI_STA_DEF"
#define WIFI_MANAGER_UNPROVISIONED_PLACEHOLDER "__UNPROVISIONED__"
#define WIFI_MANAGER_PROV_NVS_NAMESPACE "wifi_prov"
#define WIFI_MANAGER_PROV_NVS_KEY "provisioned"
/* Host-side provisioning seeds Wi-Fi credentials into this namespace; on the
 * first boot after pre-pop they're copied into esp_wifi's internal NVS via
 * esp_wifi_set_config(), then the namespace is erased. See
 * tools/build_nvs_image.py. */
#define WIFI_MANAGER_CREDS_NVS_NAMESPACE "wifi_creds"
#define WIFI_MANAGER_CREDS_NVS_KEY_SSID  "ssid"
#define WIFI_MANAGER_CREDS_NVS_KEY_PASS  "pass"

/* Reconnect backoff: attempt 1 is immediate, then BASE, 2*BASE, 4*BASE ...
 * doubling until capped at MAX. After MAX_ATTEMPTS the manager gives up and
 * reports FAILED — at that point the AP has been gone long enough that we
 * stop spamming the log; user code can re-arm via wifi_manager_connect().
 *
 * MAX is 30 min so the steady-state (last) retry happens every half hour
 * instead of hammering every 10 s. The ramp reaches the 30-min cap by ~attempt
 * 14; with 100 attempts total the manager keeps trying for ~43 h before
 * giving up. */
#define WIFI_MANAGER_RECONNECT_BACKOFF_BASE_MS 500
#define WIFI_MANAGER_RECONNECT_BACKOFF_MAX_MS  1800000   /* 30 min */
#define WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS    100

/* Auth-class retry floor (see wifi_manager_disconnect_reason_is_auth_class).
 *
 * Until 2026-09 an auth-class disconnect was "fatal": the manager dropped
 * connect_requested and never tried again. Field/bench evidence 2026-09-28
 * (DEV E8:F6:0A:B1:1F:34): a unit with CORRECT stored credentials, delivering
 * over MQTT minutes earlier, took a CPU reset (no power loss, so no deauth ever
 * reached the AP). The very first auth after the reboot was rejected
 * (reason=202 AUTH_FAIL, ~70 ms after "init -> auth") — the AP was most likely
 * still holding the pre-reset association for this MAC — and the unit then sat
 * "disconnected (provisioned: yes)" with no further attempt until an operator
 * re-ran `wifi_join` with the SAME credentials, which associated at once. Left
 * alone, only a watchdog reboot would have recovered it: the no-PUBACK watchdog
 * after >= 1 h of uptime (external power only), otherwise the next 02:00-04:00
 * nightly reboot (up to ~30 h away; battery units never trip no-PUBACK because
 * their power gate is closed). The same six reasons also fire on an ESTABLISHED
 * link (an AP reboot or kick sends AUTH_LEAVE; a marginal link times out the
 * 4-way rekey), so "fatal" stranded running units too, not just boot joins.
 *
 * On this firmware the driver cannot tell "wrong password" from "AP still has
 * stale state / RF lost the handshake" — both surface as the same codes. A unit
 * in the field has no operator to fix a password, and a genuinely wrong one
 * can only be corrected by re-provisioning, which re-arms the manager anyway.
 * So retrying costs one auth exchange per attempt, while giving up costs a
 * day of data. Auth-class failures therefore retry, but NEVER immediately (the
 * stale state that caused the rejection is still there ~0 ms later) and on a
 * floor that grows with the per-link auth-failure count: 2 s, 10 s, 30 s, then
 * 60 s, merged by max() with the ordinary exponential schedule. Retries land at
 * ~2, 12, 42, 102, 162, 222, 282, 342 s after the first rejection, spanning the
 * usual AP stale-station clearance times (PMF SA-Query ~1 s, hostapd's 300 s
 * inactivity poll) without hammering an AP that might rate-limit or lock out
 * failing clients. From attempt 9 the exponential term (64 s ... 30 min cap,
 * reached at attempt 14, ~38 min in) dominates, so a wrong
 * password converges on exactly the unreachable-AP cadence: one attempt per
 * 30 min and the same WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS give-up (~44 h),
 * which the nightly reboot pre-empts anyway.
 *
 * The wrong-password SIGNAL is kept, just decoupled from giving up:
 *  - every auth-class failure sets WIFI_MANAGER_FAILED_BIT, so an interactive
 *    wifi_manager_connect() (CLI `wifi_join`) still returns promptly, now with
 *    the manager-owned WIFI_MANAGER_ERR_AUTH_REJECTED instead of a generic
 *    ESP_FAIL, while the retry
 *    continues in the background (the typed credentials are already persisted
 *    by esp_wifi_set_config, so stopping could not restore the old ones);
 *  - the count of auth-class failures since the last GOT_IP is logged on
 *    every failure, escalating to an explicit "stored password is likely wrong"
 *    error at WIFI_MANAGER_AUTH_SUSPECT_COUNT. A transient rejection clears on
 *    the first or second retry and never reaches it. */
#define WIFI_MANAGER_AUTH_SUSPECT_COUNT 3
static const uint32_t k_wifi_manager_auth_retry_floor_ms[] = {
    2000U, 10000U, 30000U, 60000U,
};
#define WIFI_MANAGER_AUTH_RETRY_FLOOR_STEPS \
    (sizeof(k_wifi_manager_auth_retry_floor_ms) / sizeof(k_wifi_manager_auth_retry_floor_ms[0]))

typedef struct {
    EventGroupHandle_t event_group;
    esp_netif_t *sta_netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    bool handlers_registered;
    bool initialized;
    bool started;
    bool connect_requested;
    /* Armed by wifi_manager_connect() just before its own esp_wifi_disconnect(),
     * to swallow the one STA_DISCONNECTED that call produces (it must not count
     * as a reconnect attempt, nor raise FAILED under the join's wait).
     *
     * It must consume ONLY that event. 2026-09-28 bench (E8:F6:0A:B1:1F:34): a
     * `wifi_join` on a station that was NOT associated (stranded by the old
     * "fatal" AUTH_FAIL) got ESP_OK from esp_wifi_disconnect() — IDF 5.5
     * documents only ESP_OK / NOT_INIT / NOT_STARTED / FAIL for it, never
     * ESP_ERR_WIFI_NOT_CONNECT — and no event followed ("disconnected for
     * reconfigure" never logged), so the flag stayed armed through a
     * successful join. 29 min later the AP vanished (reason=200
     * BEACON_TIMEOUT); the handler ate that REAL disconnect as the reconfigure
     * one, scheduled nothing, and the unit stayed offline after the AP
     * returned. Hence two bounds:
     *  - cleared on STA_CONNECTED / GOT_IP: an association proves our
     *    disconnect is over (or never produced an event);
     *  - honoured only for WIFI_REASON_ASSOC_LEAVE (8), which the IDF Wi-Fi
     *    guide's reason table names as what the ESP station reports when it is
     *    "disconnected by esp_wifi_disconnect() and other APIs". Any other
     *    reason while armed is a genuine link event: the flag is cleared and
     *    the disconnect takes the normal retry path. The residual window is
     *    "join called, first event not yet seen": an AP-sent ASSOC_LEAVE in it
     *    is still swallowed, which is harmless because the join's own
     *    esp_wifi_connect() is already under way. */
    bool reconfigure_in_progress;
    int reconnect_count;
    /* Auth-class disconnects since the last GOT_IP (or fresh connect request).
     * Deliberately NOT reset by an interleaved transient reason: a wrong
     * password on a flaky site alternates AUTH_FAIL with NO_AP_FOUND, and the
     * wrong-password signal must still build up. */
    int auth_fail_count;
    esp_timer_handle_t reconnect_timer;
    /* ── Request ownership (2026-09 review of the retry-strand fix) ──────
     * Three contexts touch this state: the event-loop task (driver events),
     * the esp_timer task (delayed retries) and the caller of a fresh request
     * (CLI wifi_join, app_main boot). esp_timer_stop() does NOT wait for a
     * callback that is already running, so a retry that had passed its
     * connect_requested check could sit inside esp_wifi_connect() while a new
     * wifi_join reset the counters, then return and set the NEW request's
     * FAILED bit, burn its fresh budget and arm an obsolete timer.
     *
     * `lock` serialises every state transition (never held across a driver
     * call - esp_wifi_* may block on the Wi-Fi task, and nothing here may wait
     * on an event while holding it). `request_gen` is bumped by each fresh
     * request; a retry snapshots it before its driver call and discards its
     * result if it changed. `timer_gen` records which request armed the timer,
     * so a callback dispatched for a superseded request does nothing. */
    SemaphoreHandle_t lock;
    uint32_t request_gen;
    uint32_t timer_gen;
    bool timer_armed;
    char current_ssid[33];
} wifi_manager_service_t;

static const char *TAG = "wifi_manager";
static wifi_manager_service_t s_wifi = {
    .event_group = NULL,
    .sta_netif = NULL,
    .wifi_handler = NULL,
    .ip_handler = NULL,
    .handlers_registered = false,
    .initialized = false,
    .started = false,
    .connect_requested = false,
    .reconfigure_in_progress = false,
    .reconnect_count = 0,
    .auth_fail_count = 0,
    .reconnect_timer = NULL,
    .lock = NULL,
    .request_gen = 0,
    .timer_gen = 0,
    .timer_armed = false,
    .current_ssid = {0},
};

const char *wifi_manager_err_to_name(esp_err_t err)
{
    if (err == WIFI_MANAGER_ERR_AUTH_REJECTED) {
        return "WIFI_MANAGER_ERR_AUTH_REJECTED";
    }
    return esp_err_to_name(err);
}

static void wifi_manager_lock(void)
{
    (void)xSemaphoreTake(s_wifi.lock, portMAX_DELAY);
}

static void wifi_manager_unlock(void)
{
    (void)xSemaphoreGive(s_wifi.lock);
}

/* "Auth-class" = the key/identity rejections that COULD mean wrong credentials
 * (formerly "fatal"; the manager gave up on them — see the retry-floor comment
 * above for why that stranded correct credentials and what replaced it).
 * AUTH_EXPIRE and CONNECTION_FAIL were already demoted to ordinary transient
 * reasons in May 2026 because phone hotspots routinely drop the first 802.11
 * auth attempts. What remains here is no longer a stop condition, only a
 * classification: it selects the non-immediate retry floor and feeds the
 * wrong-password signal. */
static bool wifi_manager_disconnect_reason_is_auth_class(wifi_err_reason_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return true;
        default:
            return false;
    }
}

/* Backoff for reconnect attempt N (1-based). N=1 immediate, then exponential
 * from BASE, capped at MAX. */
static uint32_t wifi_manager_reconnect_delay_ms(int attempt)
{
    if (attempt <= 1) {
        return 0;
    }
    uint32_t shift = (uint32_t)(attempt - 2);
    if (shift > 16U) {
        shift = 16U;  /* guard the shift against overflow */
    }
    uint64_t delay = (uint64_t)WIFI_MANAGER_RECONNECT_BACKOFF_BASE_MS << shift;
    if (delay > WIFI_MANAGER_RECONNECT_BACKOFF_MAX_MS) {
        delay = WIFI_MANAGER_RECONNECT_BACKOFF_MAX_MS;
    }
    return (uint32_t)delay;
}

/* Delay before reconnect attempt `attempt` when the link has seen
 * `auth_fail_count` auth-class failures (0 = none: the plain exponential
 * schedule, unchanged). Otherwise max(exponential, floor[count]), the floor
 * saturating at its last step. */
static uint32_t wifi_manager_retry_delay_ms(int attempt, int auth_fail_count)
{
    uint32_t delay = wifi_manager_reconnect_delay_ms(attempt);
    if (auth_fail_count <= 0) {
        return delay;
    }
    size_t step = (size_t)(auth_fail_count - 1);
    if (step >= WIFI_MANAGER_AUTH_RETRY_FLOOR_STEPS) {
        step = WIFI_MANAGER_AUTH_RETRY_FLOOR_STEPS - 1U;
    }
    const uint32_t floor_ms = k_wifi_manager_auth_retry_floor_ms[step];
    return (delay > floor_ms) ? delay : floor_ms;
}

/* Mirror the driver's persisted STA SSID into current_ssid. The boot path
 * (wifi_manager_connect_stored_async) connects from esp_wifi's NVS config and
 * never passed through wifi_manager_apply_config, so every log line said ""
 * (2026-09-28 bench: `fatal disconnect (reason=202) for ""`). sta.ssid is a
 * 32-byte field that is NOT NUL-terminated for a 32-char SSID. */
static void wifi_manager_refresh_current_ssid(void)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return;
    }
    size_t len = 0;
    while ((len < sizeof(cfg.sta.ssid)) && (cfg.sta.ssid[len] != 0U)) {
        ++len;
    }
    memset(s_wifi.current_ssid, 0, sizeof(s_wifi.current_ssid));
    memcpy(s_wifi.current_ssid, cfg.sta.ssid, len);
}

static void wifi_manager_reconnect_timer_cb(void *arg);

/* Minimum spacing for a retry that follows a failure the driver will never
 * report as an event: esp_wifi_connect() itself returning an error (usually
 * ESP_ERR_WIFI_STATE - the driver is still busy with a previous
 * connect/scan/disconnect, which clears within a few hundred ms), or a join
 * whose connect produced nothing at all within its wait. 1 s is long enough not
 * to spin on a busy driver and short enough to cost nothing; it also keeps
 * every such retry on the timer, never the inline zero-delay path, so this
 * machinery cannot recurse. */
#define WIFI_MANAGER_CONNECT_ERROR_MIN_DELAY_MS 1000U

/* Create the reconnect timer if it does not exist yet. Called at init and
 * again lazily before every arm, so one transient allocation failure at boot
 * does not disable retries for the whole uptime. */
static esp_err_t wifi_manager_ensure_reconnect_timer(void)
{
    if (s_wifi.reconnect_timer != NULL) {
        return ESP_OK;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = &wifi_manager_reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    const esp_err_t err = esp_timer_create(&timer_args, &s_wifi.reconnect_timer);
    if (err != ESP_OK) {
        s_wifi.reconnect_timer = NULL;
    }
    return err;
}

/* Caller holds the lock. Stop any pending retry (no-op if idle/absent). */
static void wifi_manager_cancel_reconnect_locked(void)
{
    if (s_wifi.reconnect_timer != NULL) {
        esp_timer_stop(s_wifi.reconnect_timer);
    }
    s_wifi.timer_armed = false;
}

/* Caller holds the lock. End the current request explicitly: no retry owner
 * remains, so say so - connect_requested=false and FAILED, never a silent
 * connect_requested=true with nothing scheduled. The next wifi_manager_connect
 * / connect_stored_async re-arms; the sync_runner watchdog reboot is the
 * unattended backstop. */
static void wifi_manager_stop_request_locked(void)
{
    s_wifi.connect_requested = false;
    s_wifi.reconfigure_in_progress = false;
    wifi_manager_cancel_reconnect_locked();
    xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
}

/* Caller holds the lock. Arm the retry timer for the CURRENT request. Returns
 * false if no timer can be armed; the caller must then end the request
 * (wifi_manager_stop_request_locked) - there is deliberately no inline
 * "reconnect now" fallback: it bypassed the auth floor (hammering the AP) and,
 * from the retry path, recursed. */
static bool wifi_manager_arm_retry_timer_locked(uint32_t delay_ms)
{
    esp_err_t err = wifi_manager_ensure_reconnect_timer();
    if (err == ESP_OK) {
        esp_timer_stop(s_wifi.reconnect_timer);  /* idle/expired: ignored */
        err = esp_timer_start_once(s_wifi.reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    }
    if (err != ESP_OK) {
        s_wifi.timer_armed = false;
        ESP_LOGE(TAG, "reconnect timer unavailable (%s) - ending the Wi-Fi request "
                 "(re-arm with wifi_join or reboot)", esp_err_to_name(err));
        return false;
    }
    s_wifi.timer_armed = true;
    s_wifi.timer_gen = s_wifi.request_gen;
    return true;
}

/* Caller holds the lock. Count one failed attempt against
 * WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS. Once the budget is spent it ends the
 * request and returns true. Shared by every failure path (a driver
 * disconnect event, an esp_wifi_connect() error, a silent join timeout) so the
 * cap is ONE budget with ONE exhaustion rule. */
static bool wifi_manager_attempt_budget_spent_locked(int reason, esp_err_t connect_err)
{
    ++s_wifi.reconnect_count;
    if (s_wifi.reconnect_count <= WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS) {
        return false;
    }
    wifi_manager_stop_request_locked();
    ESP_LOGE(TAG,
             "Wi-Fi reconnect gave up after %d attempts (last reason=%d, connect err=%s, "
             "%d auth rejections) - \"%s\" %s",
             WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS, reason, esp_err_to_name(connect_err),
             s_wifi.auth_fail_count, s_wifi.current_ssid,
             (s_wifi.auth_fail_count > 0) ? "rejects the stored credentials" : "unreachable");
    return true;
}

/* Caller holds the lock. The current request's attempt failed in a way the
 * driver will NOT report as an event (connect call error, or silence), so
 * nothing else would ever schedule the next one: count it and arm the timer on
 * the ordinary backoff, floored at WIFI_MANAGER_CONNECT_ERROR_MIN_DELAY_MS.
 * Returns true if a retry is now owned by the timer, false if the request was
 * ended (budget spent or no timer). */
static bool wifi_manager_retry_after_silent_failure_locked(esp_err_t err, const char *what)
{
    if (wifi_manager_attempt_budget_spent_locked(0, err)) {
        return false;
    }
    uint32_t delay_ms = wifi_manager_retry_delay_ms(s_wifi.reconnect_count, 0);
    if (delay_ms < WIFI_MANAGER_CONNECT_ERROR_MIN_DELAY_MS) {
        delay_ms = WIFI_MANAGER_CONNECT_ERROR_MIN_DELAY_MS;
    }
    if (!wifi_manager_arm_retry_timer_locked(delay_ms)) {
        wifi_manager_stop_request_locked();
        return false;
    }
    ESP_LOGE(TAG, "%s (%s) - no driver event will follow; reconnect attempt %d in %u ms",
             what, esp_err_to_name(err), s_wifi.reconnect_count, (unsigned)delay_ms);
    return true;
}

/* Issue one retry for request `gen`. Called WITHOUT the lock (the driver call
 * may block), either inline for an immediate attempt or from the timer cb.
 *
 * An esp_wifi_connect() error produces NO driver event. Until 2026-09 this path
 * only logged and raised FAILED, leaving connect_requested=true with no timer
 * armed, so the bounded policy could end after one retry (Evaluator repro:
 * stored-creds boot -> AUTH_FAIL -> 2 s retry -> ESP_ERR_WIFI_STATE -> silence).
 * The error now counts as an attempt and re-arms the timer - but only if the
 * request that issued the call is still the current one. */
static void wifi_manager_issue_retry(uint32_t gen)
{
    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        return;  /* the outcome arrives as a driver event */
    }
    wifi_manager_lock();
    if ((s_wifi.request_gen != gen) || !s_wifi.connect_requested) {
        ESP_LOGW(TAG, "stale retry result (%s) for a superseded Wi-Fi request - dropped",
                 esp_err_to_name(err));
        wifi_manager_unlock();
        return;
    }
    xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
    (void)wifi_manager_retry_after_silent_failure_locked(err, "esp_wifi_connect (retry) failed");
    wifi_manager_unlock();
}

static void wifi_manager_reconnect_timer_cb(void *arg)
{
    (void)arg;
    wifi_manager_lock();
    s_wifi.timer_armed = false;
    if (!s_wifi.connect_requested || (s_wifi.timer_gen != s_wifi.request_gen)) {
        /* Superseded: a fresh request (or give-up) happened after this timer
         * was armed, and esp_timer_stop() could not recall an already
         * dispatched callback. */
        wifi_manager_unlock();
        return;
    }
    const uint32_t gen = s_wifi.request_gen;
    wifi_manager_unlock();
    wifi_manager_issue_retry(gen);
}


static esp_err_t wifi_manager_apply_seeded_creds(void);

static bool wifi_manager_config_value_is_placeholder(const char *value)
{
    return (value == NULL) ||
           (strcmp(value, WIFI_MANAGER_UNPROVISIONED_PLACEHOLDER) == 0);
}

static esp_err_t wifi_manager_ensure_event_group(void)
{
    if (s_wifi.event_group != NULL) {
        return ESP_OK;
    }

    s_wifi.event_group = xEventGroupCreate();
    if (s_wifi.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t wifi_manager_ensure_sta_netif(void)
{
    if (s_wifi.sta_netif != NULL) {
        return ESP_OK;
    }

    s_wifi.sta_netif = esp_netif_get_handle_from_ifkey(WIFI_MANAGER_STA_IFKEY);
    if (s_wifi.sta_netif == NULL) {
        s_wifi.sta_netif = esp_netif_create_default_wifi_sta();
    }

    return (s_wifi.sta_netif != NULL) ? ESP_OK : ESP_FAIL;
}

static esp_err_t wifi_manager_copy_config_string(
    char *dest,
    size_t dest_size,
    const char *src)
{
    const size_t src_len = strlen(src);
    if (src_len >= dest_size) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dest, 0, dest_size);
    memcpy(dest, src, src_len);
    return ESP_OK;
}

static esp_err_t wifi_manager_apply_config(const char *ssid, const char *password)
{
    if ((ssid == NULL) || (password == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_wifi.initialized || !s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};

    esp_err_t err = wifi_manager_copy_config_string(
        (char *)wifi_config.sta.ssid,
        sizeof(wifi_config.sta.ssid),
        ssid);
    if (err != ESP_OK) {
        return err;
    }

    err = wifi_manager_copy_config_string(
        (char *)wifi_config.sta.password,
        sizeof(wifi_config.sta.password),
        password);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config.sta.threshold.authmode =
        (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }

    return wifi_manager_copy_config_string(
        s_wifi.current_ssid,
        sizeof(s_wifi.current_ssid),
        ssid);
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        ESP_LOGI(TAG, "Wi-Fi station started");
        return;
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        const wifi_event_sta_disconnected_t *disconnected =
            (const wifi_event_sta_disconnected_t *)event_data;
        const wifi_err_reason_t reason =
            disconnected ? disconnected->reason : WIFI_REASON_UNSPECIFIED;

        xEventGroupClearBits(s_wifi.event_group, WIFI_MANAGER_CONNECTED_BIT);

        wifi_manager_lock();
        if (s_wifi.reconfigure_in_progress) {
            /* One-shot, and only for our own disconnect (see the field). */
            s_wifi.reconfigure_in_progress = false;
            if (reason == WIFI_REASON_ASSOC_LEAVE) {
                wifi_manager_unlock();
                ESP_LOGI(TAG, "Wi-Fi disconnected for reconfigure");
                return;
            }
            ESP_LOGW(TAG,
                     "reconfigure disconnect never arrived; reason=%d is a real "
                     "disconnect — handling it normally",
                     (int)reason);
        }

        if (!s_wifi.connect_requested) {
            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected (reason=%d)",
                reason);
            xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
            wifi_manager_unlock();
            return;
        }

        /* Auth-class rejections no longer stop the manager (retry-floor comment
         * at the top of this file). They raise FAILED so a waiting interactive
         * connect reports promptly, then fall through to the bounded backoff. */
        const bool auth_class = wifi_manager_disconnect_reason_is_auth_class(reason);
        if (auth_class) {
            ++s_wifi.auth_fail_count;
            xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
        }

        if (wifi_manager_attempt_budget_spent_locked((int)reason, ESP_OK)) {
            wifi_manager_unlock();
            return;
        }
        const uint32_t delay_ms =
            wifi_manager_retry_delay_ms(s_wifi.reconnect_count,
                                        auth_class ? s_wifi.auth_fail_count : 0);
        if (!auth_class) {
            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected (reason=%d), reconnect attempt %d in %u ms",
                (int)reason,
                s_wifi.reconnect_count,
                (unsigned)delay_ms);
        } else if (s_wifi.auth_fail_count < WIFI_MANAGER_AUTH_SUSPECT_COUNT) {
            ESP_LOGW(TAG,
                     "Wi-Fi auth rejected (reason=%d) by \"%s\" [%d since last IP] — "
                     "often transient (AP still holds a pre-reset association); "
                     "reconnect attempt %d in %u ms",
                     (int)reason, s_wifi.current_ssid, s_wifi.auth_fail_count,
                     s_wifi.reconnect_count, (unsigned)delay_ms);
        } else {
            ESP_LOGE(TAG,
                     "Wi-Fi auth rejected (reason=%d) by \"%s\" [%d since last IP] — "
                     "stored password is likely wrong (re-provision or wifi_join); "
                     "still retrying, attempt %d in %u ms",
                     (int)reason, s_wifi.current_ssid, s_wifi.auth_fail_count,
                     s_wifi.reconnect_count, (unsigned)delay_ms);
        }
        if (delay_ms == 0U) {
            /* Attempt 1 of a non-auth reason: immediate, issued after dropping
             * the lock (the driver call may block). */
            wifi_manager_cancel_reconnect_locked();
            const uint32_t gen = s_wifi.request_gen;
            wifi_manager_unlock();
            wifi_manager_issue_retry(gen);
            return;
        }
        if (!wifi_manager_arm_retry_timer_locked(delay_ms)) {
            wifi_manager_stop_request_locked();
        }
        wifi_manager_unlock();
        return;
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_CONNECTED)) {
        wifi_manager_lock();
        s_wifi.reconfigure_in_progress = false;   /* our disconnect is behind us */
        /* An association means the pending attempt progressed; a retry timer
         * still armed (e.g. by a join's silent-timeout path while the driver
         * was merely slow) would call esp_wifi_connect() on an associated
         * station and flap the link. */
        wifi_manager_cancel_reconnect_locked();
        wifi_manager_unlock();
        xEventGroupClearBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
        ESP_LOGI(TAG, "Associated with AP \"%s\"", s_wifi.current_ssid);
        return;
    }

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        wifi_manager_lock();
        s_wifi.reconnect_count = 0;
        s_wifi.auth_fail_count = 0;
        s_wifi.reconfigure_in_progress = false;   /* belt-and-braces with STA_CONNECTED */
        wifi_manager_cancel_reconnect_locked();
        wifi_manager_unlock();
        xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
        ESP_LOGI(TAG, "Got IP from AP");

        /* This gateway advertises a DNS server that does not actually resolve, so
         * every getaddrinfo fails with EAI_FAIL and TLS/MQTT can never connect.
         * Force a public resolver as MAIN, and keep the DHCP-provided one as BACKUP
         * so we still work on networks where public DNS is blocked but local DNS
         * resolves. Logs what DHCP gave, for diagnosis. */
        if (s_wifi.sta_netif != NULL) {
            esp_netif_dns_info_t dhcp_dns = {0};
            esp_err_t ge = esp_netif_get_dns_info(s_wifi.sta_netif, ESP_NETIF_DNS_MAIN, &dhcp_dns);
            uint32_t dhcp_addr = (ge == ESP_OK) ? dhcp_dns.ip.u_addr.ip4.addr : 0;

            esp_netif_dns_info_t main_dns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
            main_dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
            esp_netif_set_dns_info(s_wifi.sta_netif, ESP_NETIF_DNS_MAIN, &main_dns);

            esp_netif_dns_info_t bkp_dns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
            bkp_dns.ip.u_addr.ip4.addr = (dhcp_addr != 0) ? dhcp_addr : esp_ip4addr_aton("1.1.1.1");
            esp_netif_set_dns_info(s_wifi.sta_netif, ESP_NETIF_DNS_BACKUP, &bkp_dns);

            ESP_LOGW(TAG, "DNS: DHCP gave %u.%u.%u.%u — forced 8.8.8.8 as main resolver",
                     (unsigned)(dhcp_addr & 0xff), (unsigned)((dhcp_addr >> 8) & 0xff),
                     (unsigned)((dhcp_addr >> 16) & 0xff), (unsigned)((dhcp_addr >> 24) & 0xff));
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi.initialized) {
        return ESP_OK;
    }

    esp_err_t err = wifi_manager_ensure_event_group();
    if (err != ESP_OK) {
        return err;
    }
    if (s_wifi.lock == NULL) {
        s_wifi.lock = xSemaphoreCreateMutex();
        if (s_wifi.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = wifi_manager_ensure_sta_netif();
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    if (!s_wifi.handlers_registered) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &s_wifi.wifi_handler);
        if (err != ESP_OK) {
            return err;
        }

        err = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &s_wifi.ip_handler);
        if (err != ESP_OK) {
            esp_event_handler_instance_unregister(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                s_wifi.wifi_handler);
            s_wifi.wifi_handler = NULL;
            return err;
        }

        s_wifi.handlers_registered = true;
    }

    err = wifi_manager_ensure_reconnect_timer();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect timer create failed: %s - retried when first needed",
                 esp_err_to_name(err));
    }

    s_wifi.initialized = true;

    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (!s_wifi.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi.started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* Pin modem-sleep explicitly rather than rely on the IDF default. MIN_MODEM
     * lets the radio sleep between DTIM beacons while staying responsive to
     * inbound commands, and is the power-save mode the planned Phase-2 light-sleep
     * work depends on. */
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    s_wifi.started = true;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if ((ssid == NULL) || (password == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_manager_apply_config(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_lock();
    const uint32_t gen = ++s_wifi.request_gen;   /* supersedes any in-flight retry */
    s_wifi.reconnect_count = 0;
    s_wifi.auth_fail_count = 0;
    wifi_manager_cancel_reconnect_locked();
    xEventGroupClearBits(s_wifi.event_group, WIFI_MANAGER_CONNECTED_BIT | WIFI_MANAGER_FAILED_BIT);
    s_wifi.connect_requested = false;
    s_wifi.reconfigure_in_progress = true;
    wifi_manager_unlock();
    /* The reconnect timer task may be inside esp_wifi_connect() right now (an
     * unreachable AP keeps the driver busy almost continuously), and the driver
     * rejects any overlapping disconnect/connect with ESP_ERR_WIFI_STATE. Retry
     * past the transient instead of failing the join outright. */
    for (int attempt = 0; ; attempt++) {
        err = esp_wifi_disconnect();
        if ((err == ESP_OK) || (err == ESP_ERR_WIFI_NOT_CONNECT)) {
            break;
        }
        if ((err != ESP_ERR_WIFI_STATE) || (attempt >= 9)) {
            wifi_manager_lock();
            if (s_wifi.request_gen == gen) {
                s_wifi.reconfigure_in_progress = false;
            }
            wifi_manager_unlock();
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    /* Dead on IDF 5.5 (an unassociated station gets ESP_OK and no event), kept
     * for drivers that do report it. The handler, not this branch, is what
     * bounds the flag — see reconfigure_in_progress. */
    wifi_manager_lock();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        s_wifi.reconfigure_in_progress = false;
    }
    s_wifi.connect_requested = true;
    wifi_manager_unlock();

    for (int attempt = 0; ; attempt++) {
        err = esp_wifi_connect();
        if (err == ESP_OK) {
            break;
        }
        if ((err != ESP_ERR_WIFI_STATE) || (attempt >= 9)) {
            wifi_manager_lock();
            if (s_wifi.request_gen == gen) {
                s_wifi.connect_requested = false;
                /* No connect was issued, so no event can come to disarm the
                 * flag (see reconfigure_in_progress): clear it here or the NEXT
                 * real disconnect after a later successful connect is at risk. */
                s_wifi.reconfigure_in_progress = false;
            }
            wifi_manager_unlock();
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi.event_group,
        WIFI_MANAGER_CONNECTED_BIT | WIFI_MANAGER_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_MANAGER_INITIAL_CONNECT_TIMEOUT_MS));

    if ((bits & WIFI_MANAGER_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    if ((bits & WIFI_MANAGER_FAILED_BIT) != 0) {
        /* Distinct code for the operator (CLI wifi_join): the AP rejected the
         * key/identity. Not a give-up — the background retry keeps going. */
        return (s_wifi.auth_fail_count > 0) ? WIFI_MANAGER_ERR_AUTH_REJECTED : ESP_FAIL;
    }

    /* Nothing at all within the wait. Both driver calls said ESP_OK, so no
     * event may ever come (the 2026-09 review's fully silent case): if this
     * returned with no retry owner, the header's "keeps retrying" promise
     * would be false and the still-armed reconfigure latch could later eat a
     * real ASSOC_LEAVE. Disarm the latch and hand the request to the bounded
     * retry - unless a disconnect already did (timer armed) or a newer
     * request took over. */
    wifi_manager_lock();
    if (s_wifi.request_gen == gen) {
        s_wifi.reconfigure_in_progress = false;
        if (s_wifi.connect_requested && !s_wifi.timer_armed) {
            (void)wifi_manager_retry_after_silent_failure_locked(
                ESP_ERR_TIMEOUT, "wifi_join: no connect result within the wait");
        }
    }
    wifi_manager_unlock();
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_connect_configured(void)
{
    if ((CONFIG_AMBYTE_WIFI_SSID[0] == '\0') ||
        wifi_manager_config_value_is_placeholder(CONFIG_AMBYTE_WIFI_SSID)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (wifi_manager_config_value_is_placeholder(CONFIG_AMBYTE_WIFI_PASSWORD)) {
        return ESP_ERR_NOT_FOUND;
    }

    return wifi_manager_connect(CONFIG_AMBYTE_WIFI_SSID, CONFIG_AMBYTE_WIFI_PASSWORD);
}

esp_err_t wifi_manager_connect_stored_async(void)
{
    if (!s_wifi.initialized || !s_wifi.started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_manager_ensure_event_group();
    if (err != ESP_OK) {
        return err;
    }

    /* First-boot-after-pre-pop: copy host-seeded SSID/password into esp_wifi
     * NVS, then erase the seed. esp_wifi_connect() below uses the applied
     * config; on later boots the seed is gone and this is a no-op. */
    (void)wifi_manager_apply_seeded_creds();
    wifi_manager_refresh_current_ssid();

    wifi_manager_lock();
    const uint32_t gen = ++s_wifi.request_gen;
    s_wifi.reconnect_count = 0;
    s_wifi.auth_fail_count = 0;
    wifi_manager_cancel_reconnect_locked();
    xEventGroupClearBits(s_wifi.event_group,
                         WIFI_MANAGER_CONNECTED_BIT | WIFI_MANAGER_FAILED_BIT);
    s_wifi.connect_requested = true;
    wifi_manager_unlock();

    err = esp_wifi_connect();
    if (err == ESP_OK) {
        return ESP_OK;   /* connection proceeds in the background (events / reconnect) */
    }

    /* The boot caller (app_main) only logs a failure here, so a transient
     * driver error on the very first call used to strand the unit for the whole
     * boot. Configuration errors cannot heal by retrying (no/invalid stored
     * SSID = unprovisioned, which app_main already reports), so those end the
     * request; anything else is handed to the bounded retry and reported as
     * started. Contract: an error return <=> nothing will retry. */
    const bool config_error = (err == ESP_ERR_WIFI_SSID) || (err == ESP_ERR_WIFI_MODE) ||
                              (err == ESP_ERR_WIFI_NOT_INIT) ||
                              (err == ESP_ERR_WIFI_NOT_STARTED);
    bool retrying = false;
    wifi_manager_lock();
    if (s_wifi.request_gen == gen) {
        if (config_error) {
            s_wifi.connect_requested = false;
        } else {
            xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
            retrying = wifi_manager_retry_after_silent_failure_locked(
                err, "esp_wifi_connect (boot) failed");
        }
    }
    wifi_manager_unlock();
    return retrying ? ESP_OK : err;
}

esp_err_t wifi_manager_connect_stored(void)
{
    esp_err_t err = wifi_manager_connect_stored_async();
    if (err != ESP_OK) {
        return err;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi.event_group,
        WIFI_MANAGER_CONNECTED_BIT | WIFI_MANAGER_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_MANAGER_INITIAL_CONNECT_TIMEOUT_MS));

    if ((bits & WIFI_MANAGER_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    if ((bits & WIFI_MANAGER_FAILED_BIT) != 0) {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void)
{
    if (s_wifi.event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_wifi.event_group) & WIFI_MANAGER_CONNECTED_BIT) != 0;
}

/* ── Provisioning state (NVS-backed; host-seeded) ────────────────────── */

esp_err_t wifi_manager_is_provisioned(bool *out_provisioned)
{
    if (out_provisioned == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_provisioned = false;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_MANAGER_PROV_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t val = 0;
    err = nvs_get_u8(nvs, WIFI_MANAGER_PROV_NVS_KEY, &val);
    nvs_close(nvs);

    if (err == ESP_OK && val == 1) {
        *out_provisioned = true;
    }

    return ESP_OK;
}

static esp_err_t wifi_manager_mark_provisioned(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_MANAGER_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, WIFI_MANAGER_PROV_NVS_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* If host-side provisioning has seeded an SSID/password into the wifi_creds
 * namespace, copy them into esp_wifi's internal config and erase the seed so
 * the apply is one-shot. Idempotent: a no-op when the namespace is empty
 * (e.g. on every subsequent boot after the first). */
static esp_err_t wifi_manager_apply_seeded_creds(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_MANAGER_CREDS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);

    err = nvs_get_str(nvs, WIFI_MANAGER_CREDS_NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_creds: read ssid failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }

    err = nvs_get_str(nvs, WIFI_MANAGER_CREDS_NVS_KEY_PASS, pass, &pass_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pass[0] = '\0';
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_creds: read pass failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }

    ESP_LOGI(TAG, "wifi_creds: applying seeded credentials for SSID \"%s\"", ssid);
    err = wifi_manager_apply_config(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_creds: apply failed: %s — leaving seed in NVS for retry",
                 esp_err_to_name(err));
        nvs_close(nvs);
        return err;
    }

    /* Erase the seed only after a successful apply, so a partial flash can be
     * retried without re-running the host script. */
    nvs_erase_key(nvs, WIFI_MANAGER_CREDS_NVS_KEY_SSID);
    nvs_erase_key(nvs, WIFI_MANAGER_CREDS_NVS_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);

    wifi_manager_mark_provisioned();
    return ESP_OK;
}

esp_err_t wifi_manager_clear_provisioning(void)
{
    /* Clear our custom provisioned flag */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_MANAGER_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_erase_key(nvs, WIFI_MANAGER_PROV_NVS_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    /* Clear stored Wi-Fi credentials (factory reset WiFi config in NVS) */
    esp_wifi_restore();
    ESP_LOGI(TAG, "Wi-Fi provisioning cleared — rebooting");
    esp_restart();
    return ESP_OK; /* never reached */
}

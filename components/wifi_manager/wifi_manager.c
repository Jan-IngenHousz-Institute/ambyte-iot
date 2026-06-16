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
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_manager.h"

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

typedef struct {
    EventGroupHandle_t event_group;
    esp_netif_t *sta_netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    bool handlers_registered;
    bool initialized;
    bool started;
    bool connect_requested;
    bool reconfigure_in_progress;
    int reconnect_count;
    esp_timer_handle_t reconnect_timer;
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
    .reconnect_timer = NULL,
    .current_ssid = {0},
};

/* "Fatal" = a credential/configuration problem that retrying cannot fix; the
 * manager gives up and reports failure. Transient reasons (AUTH_EXPIRE,
 * CONNECTION_FAIL, NO_AP_FOUND, BEACON_TIMEOUT, ...) are deliberately NOT fatal:
 * phone hotspots routinely drop the first 802.11 auth attempts, so we back off
 * and retry instead. Only the genuine key/identity failures stay fatal. */
static bool wifi_manager_disconnect_reason_is_fatal(wifi_err_reason_t reason)
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

/* Issue one reconnect. Runs either inline (immediate retries) or from the
 * reconnect timer's task. */
static void wifi_manager_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_wifi.connect_requested) {
        return;  /* a fresh connect or fatal failure superseded this retry */
    }
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect (retry) failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
    }
}

/* Schedule a reconnect after `delay_ms`; falls back to an immediate retry if
 * the timer is unavailable or the delay is zero. */
static void wifi_manager_schedule_reconnect(uint32_t delay_ms)
{
    if ((s_wifi.reconnect_timer == NULL) || (delay_ms == 0U)) {
        wifi_manager_reconnect_timer_cb(NULL);
        return;
    }
    esp_timer_stop(s_wifi.reconnect_timer);  /* ESP_ERR_INVALID_STATE if idle — ignored */
    const esp_err_t err =
        esp_timer_start_once(s_wifi.reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect timer start failed (%s) — reconnecting now",
                 esp_err_to_name(err));
        wifi_manager_reconnect_timer_cb(NULL);
    }
}

/* Cancel any pending reconnect timer (no-op if idle or not created). */
static void wifi_manager_cancel_reconnect(void)
{
    if (s_wifi.reconnect_timer != NULL) {
        esp_timer_stop(s_wifi.reconnect_timer);
    }
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

        if (s_wifi.reconfigure_in_progress) {
            s_wifi.reconfigure_in_progress = false;
            ESP_LOGI(TAG, "Wi-Fi disconnected for reconfigure");
            return;
        }

        if (!s_wifi.connect_requested) {
            ESP_LOGW(
                TAG,
                "Wi-Fi disconnected (reason=%d)",
                reason);
            xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
            return;
        }

        if (wifi_manager_disconnect_reason_is_fatal(reason)) {
            s_wifi.connect_requested = false;
            wifi_manager_cancel_reconnect();
            xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
            ESP_LOGE(TAG, "Wi-Fi fatal disconnect (reason=%d) for \"%s\"", reason, s_wifi.current_ssid);
            return;
        }

        ++s_wifi.reconnect_count;
        if (s_wifi.reconnect_count > WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS) {
            s_wifi.connect_requested = false;
            wifi_manager_cancel_reconnect();
            xEventGroupSetBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
            ESP_LOGE(TAG,
                     "Wi-Fi reconnect gave up after %d attempts (reason=%d) — \"%s\" unreachable",
                     WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS, reason, s_wifi.current_ssid);
            return;
        }
        const uint32_t delay_ms = wifi_manager_reconnect_delay_ms(s_wifi.reconnect_count);
        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected (reason=%d), reconnect attempt %d in %u ms",
            reason,
            s_wifi.reconnect_count,
            (unsigned)delay_ms);
        wifi_manager_schedule_reconnect(delay_ms);
        return;
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_CONNECTED)) {
        xEventGroupClearBits(s_wifi.event_group, WIFI_MANAGER_FAILED_BIT);
        ESP_LOGI(TAG, "Associated with AP \"%s\"", s_wifi.current_ssid);
        return;
    }

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        s_wifi.reconnect_count = 0;
        wifi_manager_cancel_reconnect();
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

    if (s_wifi.reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &wifi_manager_reconnect_timer_cb,
            .name = "wifi_reconnect",
        };
        err = esp_timer_create(&timer_args, &s_wifi.reconnect_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reconnect timer create failed: %s — retries will be immediate",
                     esp_err_to_name(err));
            s_wifi.reconnect_timer = NULL;
        }
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

    s_wifi.reconnect_count = 0;
    wifi_manager_cancel_reconnect();
    xEventGroupClearBits(s_wifi.event_group, WIFI_MANAGER_CONNECTED_BIT | WIFI_MANAGER_FAILED_BIT);

    s_wifi.connect_requested = false;
    s_wifi.reconfigure_in_progress = true;
    err = esp_wifi_disconnect();
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_NOT_CONNECT)) {
        s_wifi.reconfigure_in_progress = false;
        return err;
    }
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        s_wifi.reconfigure_in_progress = false;
    }

    s_wifi.connect_requested = true;

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_wifi.connect_requested = false;
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

esp_err_t wifi_manager_connect_stored(void)
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

    s_wifi.reconnect_count = 0;
    wifi_manager_cancel_reconnect();
    xEventGroupClearBits(s_wifi.event_group,
                         WIFI_MANAGER_CONNECTED_BIT | WIFI_MANAGER_FAILED_BIT);
    s_wifi.connect_requested = true;

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_wifi.connect_requested = false;
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

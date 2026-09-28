/* Host harness for components/wifi_manager/wifi_manager.c.
 *
 * Compiles the PRODUCTION translation unit (included below, so its statics are
 * observable) against tests/wifi_manager_stubs/. The stubs model exactly what
 * the reconnect policy depends on: a one-shot esp_timer that the test fires by
 * hand (virtual time — the armed delay is recorded, never slept), a scripted
 * esp_wifi_connect() that queues the driver's resulting event, and an event
 * group whose WaitBits pumps those queued events before returning (standing in
 * for the event-loop task delivering them during the real 10 s wait).
 *
 * Regression anchor: 2026-09-28 DEV bench E8:F6:0A:B1:1F:34 — CPU reset, boot
 * join from stored credentials got one reason=202 AUTH_FAIL and the manager
 * never tried again (scenario boot_authfail_recovers).
 *
 * Usage: wifi_manager_host <scenario>   (see main for the list) */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_manager.c"

/* ── Stub state ─────────────────────────────────────────────────────────── */

esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t const IP_EVENT = "IP_EVENT";

static esp_event_handler_t g_wifi_handler;
static esp_event_handler_t g_ip_handler;

struct wm_stub_timer { int unused; };
static struct wm_stub_timer g_timer_obj;
static esp_timer_cb_t g_timer_cb;
static bool g_timer_armed;
static uint64_t g_timer_delay_us;

struct wm_stub_event_group { EventBits_t bits; };
static struct wm_stub_event_group g_group;

struct wm_stub_netif { int unused; };
static struct wm_stub_netif g_netif;

static uint8_t g_stored_ssid[32];
static int g_connect_calls;
static int g_set_config_calls;     /* every persisted esp_wifi config rewrite */
/* Driver association state, for esp_wifi_disconnect()'s IDF 5.5 behaviour:
 * ESP_OK either way; a STA_DISCONNECTED(ASSOC_LEAVE) event only if associated.
 * (IDF 5.5 does not document ESP_ERR_WIFI_NOT_CONNECT for it, and the
 * 2026-09-28 bench join on an unassociated station logged no event.) */
static bool g_associated;
/* Errors returned by successive esp_wifi_connect() calls (0 = ESP_OK). A
 * failing call posts NO event and does not consume the event script — that
 * is the driver behaviour the timer-callback strand hinged on. */
static esp_err_t g_connect_err[256];
static int g_connect_err_len;
static int g_connect_err_pos;
static bool g_connect_err_forever;   /* every call fails with g_connect_err[0] */

/* Script for successive esp_wifi_connect() calls: each entry is the event the
 * "driver" posts in response. >0 = STA_DISCONNECTED with that reason,
 * SCRIPT_GOT_IP = STA_CONNECTED + GOT_IP. Exhausted = the driver stays quiet. */
#define SCRIPT_GOT_IP (-1)
static int g_script[16];
static int g_script_len;
static int g_script_pos;

#define PENDING_MAX 8
static int g_pending[PENDING_MAX];
static int g_pending_len;

#define LOG_MAX 512
static char g_logs[LOG_MAX][320];
static int g_log_len;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            dump_logs();                                                       \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static void dump_logs(void)
{
    for (int i = 0; i < g_log_len; i++) {
        fprintf(stderr, "  log: %s\n", g_logs[i]);
    }
}

void wm_stub_log(char level, const char *tag, const char *fmt, ...)
{
    (void)tag;
    if (g_log_len >= LOG_MAX) return;
    char *dst = g_logs[g_log_len++];
    dst[0] = level;
    dst[1] = ' ';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst + 2, sizeof(g_logs[0]) - 2, fmt, ap);
    va_end(ap);
}

static int log_count(char level, const char *needle)
{
    int n = 0;
    for (int i = 0; i < g_log_len; i++) {
        if ((level == 0 || g_logs[i][0] == level) && strstr(g_logs[i], needle)) n++;
    }
    return n;
}

const char *esp_err_to_name(esp_err_t code) { (void)code; return "ERR"; }

esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance)
{
    (void)id; (void)arg;
    if (base == WIFI_EVENT) g_wifi_handler = handler;
    if (base == IP_EVENT) g_ip_handler = handler;
    if (instance) *instance = (void *)handler;
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance)
{
    (void)base; (void)id; (void)instance;
    return ESP_OK;
}

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { (void)key; return NULL; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &g_netif; }
esp_err_t esp_netif_get_dns_info(esp_netif_t *n, esp_netif_dns_type_t t,
                                 esp_netif_dns_info_t *info)
{
    (void)n; (void)t;
    memset(info, 0, sizeof(*info));
    return ESP_OK;
}
esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t t,
                                 esp_netif_dns_info_t *info)
{
    (void)n; (void)t; (void)info;
    return ESP_OK;
}
uint32_t esp_ip4addr_aton(const char *addr) { (void)addr; return 0x08080808U; }

void esp_restart(void) { fprintf(stderr, "unexpected esp_restart\n"); exit(2); }
void vTaskDelay(TickType_t ticks) { (void)ticks; }

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    g_timer_cb = args->callback;
    *out = &g_timer_obj;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    CHECK(t == &g_timer_obj);
    CHECK(!g_timer_armed);   /* production stops before re-arming */
    g_timer_armed = true;
    g_timer_delay_us = timeout_us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    CHECK(t == &g_timer_obj);
    if (!g_timer_armed) return ESP_ERR_INVALID_STATE;
    g_timer_armed = false;
    return ESP_OK;
}

EventGroupHandle_t xEventGroupCreate(void) { g_group.bits = 0; return &g_group; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { g->bits |= b; return g->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b)
{
    EventBits_t old = g->bits;
    g->bits &= ~b;
    return old;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t g) { return g->bits; }

static void pump_pending(void);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_all,
                                TickType_t ticks)
{
    (void)clear_on_exit; (void)wait_all; (void)ticks;
    pump_pending();          /* the event task runs during the real wait */
    return g->bits & bits;   /* nothing further within the timeout */
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)ns; (void)mode; (void)out;
    return ESP_ERR_NVS_NOT_FOUND;   /* no host-seeded creds, not provisioned */
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *o) { (void)h; (void)k; (void)o; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *l) { (void)h; (void)k; (void)o; (void)l; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; (void)k; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t m) { (void)m; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_set_ps(wifi_ps_type_t t) { (void)t; return ESP_OK; }
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c)
{
    (void)i;
    g_set_config_calls++;
    memcpy(g_stored_ssid, c->sta.ssid, sizeof(g_stored_ssid));
    return ESP_OK;
}
esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c)
{
    (void)i;
    memset(c, 0, sizeof(*c));
    memcpy(c->sta.ssid, g_stored_ssid, sizeof(g_stored_ssid));
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void)
{
    g_connect_calls++;
    esp_err_t err = ESP_OK;
    if (g_connect_err_forever) {
        err = g_connect_err[0];
    } else if (g_connect_err_pos < g_connect_err_len) {
        err = g_connect_err[g_connect_err_pos++];
    }
    if (err != ESP_OK) return err;
    if (g_script_pos < g_script_len) {
        CHECK(g_pending_len < PENDING_MAX);
        g_pending[g_pending_len++] = g_script[g_script_pos++];
    }
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    if (g_associated) {
        CHECK(g_pending_len < PENDING_MAX);
        g_pending[g_pending_len++] = WIFI_REASON_ASSOC_LEAVE;
    }
    return ESP_OK;
}
esp_err_t esp_wifi_restore(void) { return ESP_OK; }

/* ── Event delivery helpers ─────────────────────────────────────────────── */

static void ev_disconnect(int reason)
{
    wifi_event_sta_disconnected_t d;
    memset(&d, 0, sizeof(d));
    d.reason = (uint8_t)reason;
    g_associated = false;
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d);
}

static void ev_connected_got_ip(void)
{
    g_associated = true;
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    g_ip_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
}

static void pump_pending(void)
{
    while (g_pending_len > 0) {
        int ev = g_pending[0];
        memmove(g_pending, g_pending + 1, (size_t)(g_pending_len - 1) * sizeof(int));
        g_pending_len--;
        if (ev == SCRIPT_GOT_IP) ev_connected_got_ip();
        else ev_disconnect(ev);
    }
}

/* Fire the armed reconnect timer (virtual time); returns its delay in ms. */
static uint32_t fire_timer(void)
{
    CHECK(g_timer_armed);
    const uint32_t ms = (uint32_t)(g_timer_delay_us / 1000ULL);
    g_timer_armed = false;
    g_timer_cb(NULL);
    return ms;
}

static void set_stored_ssid(const char *ssid, size_t len)
{
    memset(g_stored_ssid, 0, sizeof(g_stored_ssid));
    memcpy(g_stored_ssid, ssid, len);
}

static void boot(const char *stored_ssid)
{
    set_stored_ssid(stored_ssid, strlen(stored_ssid));
    CHECK(wifi_manager_init() == ESP_OK);
    CHECK(wifi_manager_start() == ESP_OK);
    CHECK(g_wifi_handler != NULL && g_ip_handler != NULL && g_timer_cb != NULL);
}

static bool failed_bit(void) { return (g_group.bits & WIFI_MANAGER_FAILED_BIT) != 0; }

/* ── Scenarios ──────────────────────────────────────────────────────────── */

static const int k_auth_class[] = {
    WIFI_REASON_AUTH_LEAVE, WIFI_REASON_ASSOC_NOT_AUTHED,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, WIFI_REASON_802_1X_AUTH_FAILED,
    WIFI_REASON_AUTH_FAIL, WIFI_REASON_HANDSHAKE_TIMEOUT,
};
static const int k_transient[] = {
    WIFI_REASON_UNSPECIFIED, WIFI_REASON_AUTH_EXPIRE, WIFI_REASON_ASSOC_LEAVE,
    WIFI_REASON_BEACON_TIMEOUT, WIFI_REASON_NO_AP_FOUND, WIFI_REASON_ASSOC_FAIL,
    WIFI_REASON_CONNECTION_FAIL,
};
#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void scenario_classification(void)
{
    for (int i = 0; i < N_OF(k_auth_class); i++) {
        CHECK(wifi_manager_disconnect_reason_is_auth_class((wifi_err_reason_t)k_auth_class[i]));
    }
    for (int i = 0; i < N_OF(k_transient); i++) {
        CHECK(!wifi_manager_disconnect_reason_is_auth_class((wifi_err_reason_t)k_transient[i]));
    }
}

static void scenario_delay_schedule(void)
{
    /* No auth failures: byte-for-byte the pre-change exponential schedule. */
    CHECK(wifi_manager_retry_delay_ms(1, 0) == 0U);
    for (int a = 2; a <= 120; a++) {
        uint64_t legacy = (uint64_t)500U << ((a - 2) > 16 ? 16 : (a - 2));
        if (legacy > 1800000U) legacy = 1800000U;
        CHECK(wifi_manager_retry_delay_ms(a, 0) == (uint32_t)legacy);
    }
    /* Pure auth-failure run (attempt == auth count). */
    static const uint32_t expect[] = {
        2000, 10000, 30000, 60000, 60000, 60000, 60000, 60000,
        64000, 128000, 256000, 512000, 1024000, 1800000, 1800000,
    };
    for (int i = 0; i < N_OF(expect); i++) {
        CHECK(wifi_manager_retry_delay_ms(i + 1, i + 1) == expect[i]);
    }
    /* Never immediate, never below the first floor, once any auth failure. */
    for (int a = 1; a <= 120; a++) {
        for (int c = 1; c <= 120; c++) {
            const uint32_t d = wifi_manager_retry_delay_ms(a, c);
            CHECK(d >= 2000U && d <= 1800000U);
            CHECK(d >= wifi_manager_retry_delay_ms(a, 0));
        }
    }
}

/* The 2026-09-28 bench log, replayed: boot join from stored config, one
 * reason=202, then — previously — silence until a manual wifi_join. */
static void scenario_boot_authfail_recovers(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    CHECK(g_connect_calls == 1);
    CHECK(g_set_config_calls == 0);   /* stored creds used as-is */
    CHECK(strcmp(s_wifi.current_ssid, "BenchAP") == 0);   /* was "" */

    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(s_wifi.connect_requested);        /* was cleared: the strand */
    CHECK(g_timer_armed);
    CHECK(g_timer_delay_us == 2000ULL * 1000ULL);
    CHECK(g_connect_calls == 1);            /* not an immediate retry */
    CHECK(failed_bit());
    CHECK(log_count('W', "auth rejected (reason=202) by \"BenchAP\" [1 since last IP]") == 1);
    CHECK(log_count(0, "fatal") == 0);
    CHECK(log_count(0, "likely wrong") == 0);

    CHECK(fire_timer() == 2000U);
    CHECK(g_connect_calls == 2);
    /* The retry reuses the persisted config: no esp_wifi_set_config / NVS
     * rewrite anywhere on the auth-retry path. */
    CHECK(g_set_config_calls == 0);
    ev_connected_got_ip();
    CHECK(wifi_manager_is_connected());
    CHECK(!failed_bit());
    CHECK(s_wifi.reconnect_count == 0);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(!g_timer_armed);

    /* GOT_IP reset is real, not just a field: the next rejection starts the
     * ladder from the bottom again (attempt 1, first floor). */
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(s_wifi.reconnect_count == 1);
    CHECK(s_wifi.auth_fail_count == 1);
    CHECK(fire_timer() == 2000U);
    CHECK(g_set_config_calls == 0);
}

/* Genuinely wrong password: bounded, spaced retries that converge on the
 * unreachable-AP cadence and the same MAX_ATTEMPTS give-up, with the
 * wrong-password signal escalating along the way. */
static void scenario_wrong_password_bounded(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    uint64_t total_ms = 0;
    int retries = 0;
    for (;;) {
        ev_disconnect(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
        if (!s_wifi.connect_requested) break;
        const uint32_t d = fire_timer();
        CHECK(d >= 2000U);
        CHECK(d == wifi_manager_retry_delay_ms(retries + 1, retries + 1));
        total_ms += d;
        retries++;
        CHECK(retries <= WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS);
    }
    CHECK(retries == WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS);
    CHECK(g_connect_calls == 1 + WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS);
    CHECK(!s_wifi.connect_requested);   /* exhaustion still stops */
    CHECK(!g_timer_armed);
    CHECK(failed_bit());
    CHECK(g_set_config_calls == 0);
    /* A stray event after the give-up schedules nothing. */
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(!g_timer_armed);
    CHECK(g_connect_calls == 1 + WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS);
    /* ~44 h of trying, the first 8 retries inside ~6 min. */
    CHECK(total_ms > 43ULL * 3600ULL * 1000ULL && total_ms < 45ULL * 3600ULL * 1000ULL);
    CHECK(log_count('W', "often transient") == WIFI_MANAGER_AUTH_SUSPECT_COUNT - 1);
    CHECK(log_count('E', "stored password is likely wrong") ==
          WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS - (WIFI_MANAGER_AUTH_SUSPECT_COUNT - 1));
    CHECK(log_count('E', "gave up after 100 attempts") == 1);
    CHECK(log_count('E', "rejects the stored credentials") == 1);
}

/* Transient reasons interleaved with auth failures: the transient ones keep
 * their immediate/exponential schedule, and do not reset the auth count. */
static void scenario_mixed_reasons(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* attempt 1, auth 1 */
    CHECK(fire_timer() == 2000U);
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);          /* attempt 2, plain */
    CHECK(fire_timer() == 500U);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* attempt 3, auth 2 */
    CHECK(s_wifi.auth_fail_count == 2);
    CHECK(fire_timer() == 10000U);
}

/* An ESTABLISHED link kicked by the AP (AUTH_LEAVE) used to strand too. */
static void scenario_established_auth_leave(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected());

    ev_disconnect(WIFI_REASON_AUTH_LEAVE);
    CHECK(!wifi_manager_is_connected());
    CHECK(s_wifi.connect_requested);
    CHECK(fire_timer() == 2000U);
    CHECK(g_connect_calls == 2);
}

/* Non-auth reasons: unchanged (attempt 1 immediate, then 500 ms, 1 s ...). */
static void scenario_non_auth_unchanged(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);
    CHECK(g_connect_calls == 2);     /* immediate, inline */
    CHECK(!g_timer_armed);
    CHECK(!failed_bit());
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);
    CHECK(fire_timer() == 500U);
    ev_disconnect(WIFI_REASON_BEACON_TIMEOUT);
    CHECK(fire_timer() == 1000U);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(log_count(0, "auth rejected") == 0);
}

/* Interactive wifi_join with a wrong password: prompt, distinct result; the
 * manager keeps retrying in the background instead of going silent. */
static void scenario_join_wrong_password(void)
{
    boot("OldAP");
    g_script[0] = WIFI_REASON_AUTH_FAIL;
    g_script_len = 1;
    const esp_err_t err = wifi_manager_connect("LabAP", "wrong-pass");
    CHECK(err == WIFI_MANAGER_ERR_AUTH_REJECTED);
    CHECK(err == ESP_ERR_WIFI_PASSWORD);
    CHECK(strcmp(s_wifi.current_ssid, "LabAP") == 0);
    CHECK(s_wifi.connect_requested);
    CHECK(g_timer_armed && g_timer_delay_us == 2000ULL * 1000ULL);
}

static void scenario_join_timeout_and_success(void)
{
    boot("OldAP");
    g_script[0] = WIFI_REASON_NO_AP_FOUND;
    g_script_len = 1;
    CHECK(wifi_manager_connect("GoneAP", "pw") == ESP_ERR_TIMEOUT);   /* unchanged */

    g_script[1] = SCRIPT_GOT_IP;
    g_script_len = 2;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(s_wifi.auth_fail_count == 0);
}

/* Second 2026-09-28 bench strand: wifi_join on an UNASSOCIATED station.
 * esp_wifi_disconnect() returns ESP_OK and posts no event, so the reconfigure
 * flag used to stay armed through the successful join and later swallow the
 * AP's real disconnect (reason=200 BEACON_TIMEOUT) with no reconnect. */
static void scenario_join_unassociated_then_beacon_timeout(void)
{
    boot("BenchAP");
    CHECK(!g_associated);
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "right-pass") == ESP_OK);
    CHECK(wifi_manager_is_connected());
    CHECK(!s_wifi.reconfigure_in_progress);   /* association disarmed it */
    CHECK(log_count(0, "disconnected for reconfigure") == 0);
    const int calls = g_connect_calls;

    ev_disconnect(WIFI_REASON_BEACON_TIMEOUT);   /* ~29 min later: AP gone */
    CHECK(s_wifi.connect_requested);
    CHECK(s_wifi.reconnect_count == 1);
    CHECK(g_connect_calls == calls + 1);         /* attempt 1: immediate */
    CHECK(log_count(0, "disconnected for reconfigure") == 0);
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);      /* AP still down */
    CHECK(fire_timer() == 500U);
    CHECK(g_connect_calls == calls + 2);
    ev_connected_got_ip();                       /* AP back: recovers */
    CHECK(wifi_manager_is_connected());
}

/* Even if no association ever clears it, a flag left armed by an unassociated
 * join yields to the first non-ASSOC_LEAVE reason instead of eating it. */
static void scenario_join_unassociated_connect_fails(void)
{
    boot("BenchAP");
    g_script[0] = WIFI_REASON_BEACON_TIMEOUT;
    g_script_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_TIMEOUT);
    CHECK(!s_wifi.reconfigure_in_progress);
    CHECK(s_wifi.connect_requested);
    CHECK(s_wifi.reconnect_count == 1);          /* went through the retry path */
    CHECK(log_count('W', "reconfigure disconnect never arrived; reason=200") == 1);
    CHECK(log_count(0, "disconnected for reconfigure") == 0);
}

/* Normal join while ASSOCIATED: exactly one ASSOC_LEAVE is ours and is
 * suppressed; the join connects; a later ASSOC_LEAVE is a real event. */
static void scenario_join_while_connected(void)
{
    boot("OldAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script[1] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected() && g_associated);
    const int calls = g_connect_calls;

    g_script_len = 2;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(log_count('I', "disconnected for reconfigure") == 1);
    CHECK(s_wifi.reconnect_count == 0);          /* not counted as an attempt */
    CHECK(!g_timer_armed);
    CHECK(g_connect_calls == calls + 1);         /* only the join's own connect */
    CHECK(!s_wifi.reconfigure_in_progress);
    CHECK(wifi_manager_is_connected());
    CHECK(g_set_config_calls == 1);              /* the join's one rewrite */

    ev_disconnect(WIFI_REASON_ASSOC_LEAVE);      /* AP-sent, well after the join */
    CHECK(log_count('I', "disconnected for reconfigure") == 1);
    CHECK(s_wifi.connect_requested);
    CHECK(s_wifi.reconnect_count == 1);
    CHECK(g_connect_calls == calls + 2);
}

/* Evaluator repro: an esp_wifi_connect() ERROR inside the retry timer posts
 * no event, and used to leave connect_requested=true with no timer armed —
 * the whole bounded policy ended after one retry. Covers ESP_ERR_WIFI_STATE
 * (driver busy) and a generic ESP_FAIL, both before any driver event. */
static void scenario_retry_connect_error_reschedules(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* attempt 1: 2 s floor */
    g_connect_err[0] = ESP_ERR_WIFI_STATE;
    g_connect_err[1] = ESP_FAIL;
    g_connect_err_len = 2;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;

    int calls = g_connect_calls;
    CHECK(fire_timer() == 2000U);                    /* -> WIFI_STATE, no event */
    CHECK(g_connect_calls == calls + 1);             /* one call, no spin */
    CHECK(s_wifi.connect_requested);
    CHECK(g_timer_armed);                            /* was: nothing armed */
    CHECK(s_wifi.reconnect_count == 2);              /* counted as an attempt */
    CHECK(log_count('E', "esp_wifi_connect (retry) failed") == 1);

    calls = g_connect_calls;
    CHECK(fire_timer() == 1000U);                    /* floor over 500 ms; -> ESP_FAIL */
    CHECK(g_connect_calls == calls + 1);
    CHECK(g_timer_armed);
    CHECK(s_wifi.reconnect_count == 3);

    CHECK(fire_timer() == 1000U);                    /* exp(3)=1000; now succeeds */
    CHECK(!g_timer_armed);
    pump_pending();                                  /* GOT_IP */
    CHECK(wifi_manager_is_connected());
    CHECK(s_wifi.reconnect_count == 0 && s_wifi.auth_fail_count == 0);
    CHECK(g_set_config_calls == 0);
}

/* The inline (attempt-1, zero-delay) path must not recurse on a connect
 * error: exactly one esp_wifi_connect() per handler call, then a timer. */
static void scenario_inline_connect_error_no_recursion(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    g_connect_err[0] = ESP_ERR_WIFI_STATE;
    g_connect_err_forever = true;
    const int calls = g_connect_calls;
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);          /* attempt 1: inline retry */
    CHECK(g_connect_calls == calls + 1);
    CHECK(g_timer_armed);
    CHECK(g_timer_delay_us == 1000ULL * 1000ULL);
    CHECK(s_wifi.reconnect_count == 2);
}

/* A driver that keeps refusing esp_wifi_connect() still exhausts the ONE
 * shared budget: exactly MAX_ATTEMPTS attempts, never faster than 1 s, then
 * a clean stop. */
static void scenario_connect_error_exhausts_budget(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_BEACON_TIMEOUT);       /* attempt 1 inline, OK */
    g_connect_err[0] = ESP_FAIL;
    g_connect_err_forever = true;
    ev_disconnect(WIFI_REASON_BEACON_TIMEOUT);       /* attempt 2: 500 ms timer */
    int fires = 0;
    while (g_timer_armed) {
        const int calls = g_connect_calls;
        const uint32_t d = fire_timer();
        CHECK(g_connect_calls == calls + 1);
        CHECK(fires == 0 || d >= 1000U);
        fires++;
        CHECK(fires <= WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS);
    }
    CHECK(s_wifi.reconnect_count == WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS + 1);
    CHECK(!s_wifi.connect_requested);
    CHECK(failed_bit());
    CHECK(log_count('E', "gave up after 100 attempts") == 1);
    CHECK(log_count('E', "unreachable") == 1);
}

/* wifi_join whose own esp_wifi_connect() fails terminally (no event can
 * follow) must not leave the reconfigure flag armed for a later disconnect. */
static void scenario_join_connect_error_clears_flag(void)
{
    boot("BenchAP");                                 /* unassociated: no event */
    g_connect_err[0] = ESP_ERR_WIFI_CONN;
    g_connect_err_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_WIFI_CONN);
    CHECK(!s_wifi.reconfigure_in_progress);
    CHECK(!s_wifi.connect_requested);

    /* Busy driver for all ten tries: same guarantee. */
    g_connect_err[0] = ESP_ERR_WIFI_STATE;
    g_connect_err_forever = true;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_WIFI_STATE);
    CHECK(!s_wifi.reconfigure_in_progress);
    g_connect_err_forever = false;
    g_connect_err_len = 0;

    /* Later: a good join, then a real disconnect is handled, not swallowed. */
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_OK);
    const int calls = g_connect_calls;
    ev_disconnect(WIFI_REASON_BEACON_TIMEOUT);
    CHECK(g_connect_calls == calls + 1);
    CHECK(log_count(0, "disconnected for reconfigure") == 0);
}

/* A 32-char SSID fills sta.ssid with no NUL; current_ssid must stay bounded. */
static void scenario_ssid_32_chars(void)
{
    static const char ssid32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    CHECK(strlen(ssid32) == 32);
    boot("x");
    set_stored_ssid(ssid32, 32);
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    CHECK(strcmp(s_wifi.current_ssid, ssid32) == 0);
    CHECK(s_wifi.current_ssid[32] == '\0');
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } k[] = {
        { "classification", scenario_classification },
        { "delay_schedule", scenario_delay_schedule },
        { "boot_authfail_recovers", scenario_boot_authfail_recovers },
        { "wrong_password_bounded", scenario_wrong_password_bounded },
        { "mixed_reasons", scenario_mixed_reasons },
        { "established_auth_leave", scenario_established_auth_leave },
        { "non_auth_unchanged", scenario_non_auth_unchanged },
        { "join_wrong_password", scenario_join_wrong_password },
        { "join_timeout_and_success", scenario_join_timeout_and_success },
        { "ssid_32_chars", scenario_ssid_32_chars },
        { "join_unassociated_then_beacon_timeout",
          scenario_join_unassociated_then_beacon_timeout },
        { "join_unassociated_connect_fails", scenario_join_unassociated_connect_fails },
        { "join_while_connected", scenario_join_while_connected },
        { "retry_connect_error_reschedules", scenario_retry_connect_error_reschedules },
        { "inline_connect_error_no_recursion", scenario_inline_connect_error_no_recursion },
        { "connect_error_exhausts_budget", scenario_connect_error_exhausts_budget },
        { "join_connect_error_clears_flag", scenario_join_connect_error_clears_flag },
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scenario>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(argv[1], k[i].name) == 0) {
            k[i].fn();
            printf("PASS %s\n", k[i].name);
            return 0;
        }
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}

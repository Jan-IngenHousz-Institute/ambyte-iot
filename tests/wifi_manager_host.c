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

/* Epoch internals through one indirection, so this same harness also builds
 * against production code that predates the recovery epoch (RED evidence):
 * there the epoch never runs, and every scenario that depends on it fails. */
#ifdef WIFI_MANAGER_EPOCH_DEADLINE_MS
#define HAS_EPOCH 1
#define EPOCH_STATE (s_wifi.epoch_state)
#define EPOCH_IDLE WIFI_MANAGER_EPOCH_IDLE
#define EPOCH_STOPPING WIFI_MANAGER_EPOCH_STOPPING
#define EPOCH_RESUMING WIFI_MANAGER_EPOCH_RESUMING
#define EPOCH_STARTING WIFI_MANAGER_EPOCH_STARTING
#define REBOOT_NEEDED (s_wifi.reboot_needed)
#define EPOCH_TIMER_ARMED_FLAG (s_wifi.epoch_timer_armed)
#else
#define HAS_EPOCH 0
#define EPOCH_STATE 0
#define EPOCH_IDLE 0
#define EPOCH_STOPPING 1
#define EPOCH_RESUMING 2
#define EPOCH_STARTING 3
#define REBOOT_NEEDED false
#define EPOCH_TIMER_ARMED_FLAG false
typedef struct { uint32_t epoch_gen; uint8_t kind; } wifi_manager_barrier_t;
esp_event_base_t const WIFI_MANAGER_EVENT = "WIFI_MANAGER_EVENT";
#define WIFI_MANAGER_EVENT_EPOCH_BARRIER 1
#endif

/* ── Stub state ─────────────────────────────────────────────────────────── */

esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t const IP_EVENT = "IP_EVENT";

static esp_event_handler_t g_wifi_handler;
static esp_event_handler_t g_ip_handler;
static esp_event_handler_t g_mgr_handler;    /* the manager's own barrier base */

struct wm_stub_timer { esp_timer_cb_t cb; bool armed; uint64_t delay_us; };
static struct wm_stub_timer g_tm[2];           /* [0] reconnect, [1] recovery epoch */
#define g_timer_cb (g_tm[0].cb)
#define g_timer_armed (g_tm[0].armed)
#define g_timer_delay_us (g_tm[0].delay_us)
#define g_epoch_timer_armed (g_tm[1].armed)
static int g_timer_create_fail;      /* next N creates fail; -1 = always */
static bool g_timer_start_fail;
/* The connect wait returns its bit snapshot BEFORE the pending events are
 * delivered: models GOT_IP / STA_CONNECTED landing between the wait returning
 * and the caller taking the lock. */
static bool g_wait_snapshot_first;

/* Single-threaded harness: a second take is a self-deadlock on the real
 * (non-recursive) mutex, and driver calls must never run under the lock. */
struct wm_stub_mutex { int unused; };
static struct wm_stub_mutex g_mutex_obj;
static bool g_lock_held;

struct wm_stub_event_group { EventBits_t bits; };
static struct wm_stub_event_group g_group;

struct wm_stub_netif { int unused; };
static struct wm_stub_netif g_netif;

static uint8_t g_stored_ssid[32];
static int g_connect_calls;
static esp_err_t g_set_config_err;   /* returned (and nothing saved) if set */
/* Runs INSIDE the next esp_wifi_connect() call, which then returns
 * g_hook_outer_err: models another task acting while that call blocks. */
static void (*g_connect_hook)(void);
static esp_err_t g_hook_outer_err;
static int g_set_config_calls;     /* every persisted esp_wifi config rewrite */
/* Driver association state, for esp_wifi_disconnect()'s IDF 5.5 behaviour:
 * ESP_OK either way; a STA_DISCONNECTED(ASSOC_LEAVE) event only if associated.
 * (IDF 5.5 does not document ESP_ERR_WIFI_NOT_CONNECT for it, and the
 * 2026-09-28 bench join on an unassociated station logged no event.) */
static bool g_associated;
/* Driver model of ONE outstanding attempt: set when esp_wifi_connect() returns
 * ESP_OK with no scripted outcome (a silent attempt), cleared when any outcome
 * event is delivered. While set, esp_wifi_connect() returns ESP_ERR_WIFI_STATE
 * (the real driver runs one attempt at a time) and esp_wifi_disconnect()
 * aborts it and reports its outcome (IDF: "disconnect during connect"). */
static bool g_driver_pending;
/* The abort's outcome event is delayed: the test delivers it later (e.g. after
 * a join's drain timed out). */
static bool g_defer_abort_event;
/* The event task runs the abort's outcome before esp_wifi_disconnect()
 * returns to its caller (models it landing before the caller re-locks). */
static bool g_disconnect_delivers_now;
static int g_disconnect_calls;
/* Runs INSIDE the next esp_wifi_disconnect() call (another task acting while
 * the manager's kick is unlocked in the driver). */
static void (*g_disconnect_hook)(void);

/* -- Driver behaviour observed on hardware (DEV 28:37:2F:FF:E7:04, 2026-09-29) --
 * esp_wifi_set_config() on an ASSOCIATED station drops the link itself
 * ("run -> init (0x100)", STA_DISCONNECTED reason=1 UNSPECIFIED). Always on:
 * it is what IDF 5.5 does. */
static bool g_set_config_delivers_now;   /* event task runs it inside the call */
/* A connect issued while set_config is dropping the link (the event task ran
 * the reason=1 handler inside set_config, as on the bench) is accepted and
 * then dropped: no attempt, no outcome, ever. */
static bool g_drop_connect_in_set_config;
static bool g_in_set_config;
/* The next N esp_wifi_connect() calls are ACCEPTED (ESP_OK) and then dropped:
 * the driver holds no attempt and never reports an outcome. */
static int g_connect_dropped;
/* A kick (esp_wifi_disconnect) on a station with no association returns
 * ESP_ERR_WIFI_STATE and posts no event. */
static bool g_kick_state_error;
/* esp_wifi_disconnect() on an ASSOCIATED station drops it without posting
 * STA_DISCONNECTED. */
static bool g_disconnect_silent;
static bool g_disconnect_ignored;          /* the kick leaves the association up */
static int64_t g_now_us;                 /* virtual clock (esp_timer_get_time) */

/* -- STA stop/start (recovery epoch) driver model -------------------------------
 * esp_wifi_stop() on an associated station first posts STA_DISCONNECTED
 * (ASSOC_LEAVE), frees the station (a pending attempt is gone, no outcome),
 * then posts STA_STOP; esp_wifi_start() posts STA_START. Knobs model errors,
 * lost events, events handled before the call returns, and other tasks acting
 * while the call is inside the driver. */
static bool g_wifi_started;
static int g_stop_calls;
static int g_start_calls;
static esp_err_t g_stop_err;
static esp_err_t g_start_err;
static bool g_lose_sta_stop;
static bool g_lose_sta_start;
static bool g_stop_delivers_now;           /* STA_STOP handled before stop() returns */
static void (*g_stop_hook)(void);
static void (*g_start_hook)(void);
static esp_err_t g_barrier_post_err;
static bool g_barrier_delivers_now;        /* barrier handled before the poster re-locks */
static int g_barrier_posts;
static bool g_lose_barrier;                /* post "succeeds" but the event is dropped */
static bool g_sta_stop_after_barrier;      /* the driver's STA_STOP lands after our fence */
static bool g_stop_pending_sta_stop;
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
#define SCRIPT_CONNECTED_ONLY (-2)   /* associated; DHCP still pending */
#define SCRIPT_STA_STOP (-3)
#define SCRIPT_STA_START (-4)
#define SCRIPT_BARRIER (-5)          /* manager barrier; payload in g_pending_data */
#define SCRIPT_GOT_IP_ONLY (-6)      /* a GOT_IP with no association (lwIP producer) */
#define SCRIPT_STALE_CONNECTED (-7)  /* an old epoch's STA_CONNECTED (no driver state) */
static int g_script[16];
static int g_script_len;
static int g_script_pos;

#define PENDING_MAX 16
static int g_pending[PENDING_MAX];
static uint32_t g_pending_data[PENDING_MAX][2];
static int g_pending_len;

static void push_event(int ev, uint32_t d0, uint32_t d1)
{
    if (g_pending_len >= PENDING_MAX) {
        fprintf(stderr, "pending queue overflow\n");
        exit(1);
    }
    g_pending[g_pending_len] = ev;
    g_pending_data[g_pending_len][0] = d0;
    g_pending_data[g_pending_len][1] = d1;
    g_pending_len++;
}

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
int64_t esp_timer_get_time(void) { return g_now_us; }

esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance)
{
    (void)id; (void)arg;
    if (base == WIFI_EVENT) g_wifi_handler = handler;
    else if (base == IP_EVENT) g_ip_handler = handler;
    else g_mgr_handler = handler;
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

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &g_mutex_obj; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks)
{
    (void)ticks;
    CHECK(m == &g_mutex_obj);
    CHECK(!g_lock_held);
    g_lock_held = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{
    CHECK(m == &g_mutex_obj);
    CHECK(g_lock_held);
    g_lock_held = false;
    return pdTRUE;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    if (g_timer_create_fail != 0) {
        if (g_timer_create_fail > 0) g_timer_create_fail--;
        return ESP_ERR_NO_MEM;
    }
    struct wm_stub_timer *tm = &g_tm[strcmp(args->name, "wifi_epoch") == 0 ? 1 : 0];
    tm->cb = args->callback;
    *out = tm;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    CHECK(t == &g_tm[0] || t == &g_tm[1]);
    CHECK(!t->armed);   /* production stops before re-arming */
    if (g_timer_start_fail) return ESP_FAIL;
    t->armed = true;
    t->delay_us = timeout_us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    CHECK(t == &g_tm[0] || t == &g_tm[1]);
    if (!t->armed) return ESP_ERR_INVALID_STATE;
    t->armed = false;
    return ESP_OK;
}

static void pump_pending(void);
static void fire_epoch_timer(void);
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data,
                         size_t size, TickType_t ticks)
{
    (void)base; (void)id;
    CHECK(!g_lock_held);                 /* posted outside the manager lock */
    CHECK(ticks == 0);                   /* never a blocking post */
    CHECK(size == sizeof(wifi_manager_barrier_t));
    g_barrier_posts++;
    if (g_barrier_post_err != ESP_OK) return g_barrier_post_err;
    const wifi_manager_barrier_t *b = (const wifi_manager_barrier_t *)data;
    if (!g_lose_barrier) push_event(SCRIPT_BARRIER, b->epoch_gen, b->kind);
    if (g_stop_pending_sta_stop) {                   /* STA_STOP behind our fence */
        g_stop_pending_sta_stop = false;
        push_event(SCRIPT_STA_STOP, 0, 0);
    }
    if (g_barrier_delivers_now) pump_pending();
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

EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_all,
                                TickType_t ticks)
{
    (void)clear_on_exit; (void)wait_all; (void)ticks;
    if (g_wait_snapshot_first && (bits & WIFI_MANAGER_CONNECTED_BIT) != 0) {
        const EventBits_t snapshot = g->bits & bits;
        pump_pending();      /* events land after the wait returned */
        return snapshot;
    }
    pump_pending();          /* the event task runs during the real wait */
    while ((EPOCH_STATE == EPOCH_RESUMING) && g_epoch_timer_armed) {
        fire_epoch_timer();  /* the esp_timer task runs the epoch's resume step */
        pump_pending();
    }
    if ((g->bits & bits) == 0) {
        g_now_us += (int64_t)ticks * 1000;   /* the wait ran to its timeout */
    }
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
esp_err_t esp_wifi_start(void)
{
    CHECK(!g_lock_held);
    g_start_calls++;
    if (g_start_hook != NULL) {
        void (*hook)(void) = g_start_hook;
        g_start_hook = NULL;
        hook();
    }
    if (g_start_err != ESP_OK) return g_start_err;
    g_wifi_started = true;
    if (!g_lose_sta_start) push_event(SCRIPT_STA_START, 0, 0);
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    CHECK(!g_lock_held);
    g_stop_calls++;
    if (g_stop_hook != NULL) {
        void (*hook)(void) = g_stop_hook;
        g_stop_hook = NULL;
        hook();
    }
    if (g_stop_err != ESP_OK) return g_stop_err;
    if (g_associated) {
        push_event(WIFI_REASON_ASSOC_LEAVE, 0, 0);   /* stop while associated */
        g_associated = false;
    }
    g_driver_pending = false;                        /* control block freed */
    g_wifi_started = false;
    if (g_sta_stop_after_barrier) {
        g_stop_pending_sta_stop = true;
    } else if (!g_lose_sta_stop) {
        push_event(SCRIPT_STA_STOP, 0, 0);
    }
    if (g_stop_delivers_now) pump_pending();
    return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t t) { (void)t; return ESP_OK; }
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c)
{
    (void)i;
    CHECK(!g_lock_held);
    g_set_config_calls++;
    if (g_set_config_err != ESP_OK) return g_set_config_err;
    if (g_associated) {                  /* IDF drops the link inside set_config */
        g_associated = false;
        push_event(WIFI_REASON_UNSPECIFIED, 0, 0);
        if (g_set_config_delivers_now) {
            g_in_set_config = true;
            pump_pending();
            g_in_set_config = false;
        }
    }
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
    CHECK(!g_lock_held);
    /* IDF 5.5 esp_wifi.h: "If station interface is connected to an AP, call
     * esp_wifi_disconnect to disconnect" first. Doing otherwise is a bug. */
    CHECK(!g_associated);
    g_connect_calls++;
    if (!g_wifi_started) return ESP_ERR_WIFI_NOT_STARTED;
    /* A connect issued while another connect call is still inside the driver
     * (the hook below) is refused, as the busy driver would. */
    static bool s_call_in_progress;
    if (s_call_in_progress) return ESP_ERR_WIFI_STATE;
    if (g_connect_hook != NULL) {
        void (*hook)(void) = g_connect_hook;
        g_connect_hook = NULL;
        s_call_in_progress = true;
        hook();
        s_call_in_progress = false;
        return g_hook_outer_err;
    }
    if (g_driver_pending) return ESP_ERR_WIFI_STATE;
    esp_err_t err = ESP_OK;
    if (g_connect_err_forever) {
        err = g_connect_err[0];
    } else if (g_connect_err_pos < g_connect_err_len) {
        err = g_connect_err[g_connect_err_pos++];
    }
    if (err != ESP_OK) return err;
    if (g_connect_dropped > 0) {         /* accepted, then silently dropped */
        g_connect_dropped--;
        return ESP_OK;
    }
    if (g_in_set_config && g_drop_connect_in_set_config) {
        return ESP_OK;                   /* raced set_config's own disconnect */
    }
    if (g_script_pos < g_script_len) {
        push_event(g_script[g_script_pos++], 0, 0);
    } else {
        g_driver_pending = true;
    }
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    CHECK(!g_lock_held);
    g_disconnect_calls++;
    if (!g_wifi_started) return ESP_ERR_WIFI_NOT_STARTED;
    if (g_disconnect_hook != NULL) {
        void (*hook)(void) = g_disconnect_hook;
        g_disconnect_hook = NULL;
        hook();
    }
    if (g_associated && g_disconnect_ignored) {
        /* no effect */
    } else if (g_associated) {
        if (!g_disconnect_silent) {
            push_event(WIFI_REASON_ASSOC_LEAVE, 0, 0);
        }
        g_associated = false;        /* driver state changes now; event follows */
    } else if (g_driver_pending) {
        /* A deferred abort means the driver is still working on the attempt:
         * it stays busy (connect -> ESP_ERR_WIFI_STATE) until its outcome is
         * delivered by the test. */
        if (!g_defer_abort_event) {
            g_driver_pending = false;
            push_event(WIFI_REASON_ASSOC_LEAVE, 0, 0);
        }
    } else if (g_kick_state_error) {
        if (g_disconnect_delivers_now) pump_pending();
        return ESP_ERR_WIFI_STATE;       /* idle station: no event */
    }
    if (g_disconnect_delivers_now) pump_pending();
    return ESP_OK;
}
esp_err_t esp_wifi_restore(void) { return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    CHECK(!g_lock_held);
    memset(ap_info, 0, sizeof(*ap_info));
    return g_associated ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT;
}

/* ── Event delivery helpers ─────────────────────────────────────────────── */

/* Mirrors main/app_main.c on_got_ip (registered after wifi_manager_init, so it
 * runs after the manager's handler): link services start only when the
 * manager says the link is current. test_wifi_manager.py asserts app_main.c
 * performs the same gate before SNTP/MQTT. */
static int g_app_link_starts;
static int g_app_link_refusals;
static void app_on_got_ip(void)
{
    if (wifi_manager_link_is_current()) {
        g_app_link_starts++;
    } else {
        g_app_link_refusals++;
    }
}

static void ev_got_ip(void)
{
    g_ip_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    app_on_got_ip();
}

static void ev_stale_disconnect(int reason)
{
    wifi_event_sta_disconnected_t d;
    memset(&d, 0, sizeof(d));
    d.reason = (uint8_t)reason;
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d);
}

static void ev_disconnect(int reason)
{
    wifi_event_sta_disconnected_t d;
    memset(&d, 0, sizeof(d));
    d.reason = (uint8_t)reason;
    g_associated = false;
    g_driver_pending = false;
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d);
}

static void ev_connected_only(void)
{
    g_associated = true;
    g_driver_pending = false;
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
}

static void ev_connected_got_ip(void)
{
    g_associated = true;
    g_driver_pending = false;
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ev_got_ip();
}

/* Deliver ONE queued event (the event-loop task dispatching it). */
static bool pump_one(void)
{
    if (g_pending_len == 0) return false;
    const int ev = g_pending[0];
    const uint32_t d0 = g_pending_data[0][0];
    const uint32_t d1 = g_pending_data[0][1];
    memmove(g_pending, g_pending + 1, (size_t)(g_pending_len - 1) * sizeof(int));
    memmove(g_pending_data, g_pending_data + 1,
            (size_t)(g_pending_len - 1) * sizeof(g_pending_data[0]));
    g_pending_len--;
    if (ev == SCRIPT_GOT_IP) {
        ev_connected_got_ip();
    } else if (ev == SCRIPT_CONNECTED_ONLY) {
        ev_connected_only();
    } else if (ev == SCRIPT_GOT_IP_ONLY) {
        ev_got_ip();
    } else if (ev == SCRIPT_STALE_CONNECTED) {
        g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    } else if (ev == SCRIPT_STA_STOP) {
        g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    } else if (ev == SCRIPT_STA_START) {
        g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    } else if (ev == SCRIPT_BARRIER) {
        const wifi_manager_barrier_t b = { .epoch_gen = d0, .kind = (uint8_t)d1 };
        CHECK(g_mgr_handler != NULL);
        g_mgr_handler(NULL, WIFI_MANAGER_EVENT, WIFI_MANAGER_EVENT_EPOCH_BARRIER, (void *)&b);
    } else {
        ev_disconnect(ev);
    }
    return true;
}

static void pump_pending(void)
{
    while (pump_one()) {
    }
}

/* Run the recovery epoch's deferred steps (esp_timer task) until it settles:
 * deliver events, fire the epoch timer (resume / deadline). Bounded. */
static void fire_epoch_timer(void)
{
    CHECK(g_epoch_timer_armed);
    g_epoch_timer_armed = false;
    g_now_us += (int64_t)g_tm[1].delay_us;
    g_tm[1].cb(NULL);
}
static void run_epoch(void)
{
    for (int i = 0; i < 16; i++) {
        pump_pending();
        if (!g_epoch_timer_armed || EPOCH_STATE == EPOCH_IDLE) {
            pump_pending();
            return;
        }
        if (EPOCH_STATE != EPOCH_RESUMING) return;   /* awaiting events */
        fire_epoch_timer();
    }
    CHECK(!"epoch did not settle");
}

/* Expire the armed one-shot WITHOUT running its callback: IDF has set its
 * alarm to 0 (esp_timer_stop() now returns ESP_ERR_INVALID_STATE) and the
 * dispatch is committed; the test runs g_timer_cb later. */
static void expire_timer(void)
{
    CHECK(g_timer_armed);
    g_timer_armed = false;
}

/* Fire the armed reconnect timer (virtual time); returns its delay in ms. */
static uint32_t fire_timer(void)
{
    CHECK(g_timer_armed);
    const uint32_t ms = (uint32_t)(g_timer_delay_us / 1000ULL);
    g_timer_armed = false;
    g_now_us += (int64_t)g_timer_delay_us;   /* virtual time advances to expiry */
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
    CHECK(g_wifi_handler != NULL && g_ip_handler != NULL && (!HAS_EPOCH || g_mgr_handler != NULL));
    pump_pending();                                  /* the boot STA_START */
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
    CHECK(err != ESP_ERR_WIFI_PASSWORD);   /* never a driver code */
    CHECK(strcmp(s_wifi.current_ssid, "LabAP") == 0);
    CHECK(s_wifi.connect_requested);
    CHECK(g_timer_armed && g_timer_delay_us == 2000ULL * 1000ULL);
}

static void scenario_join_timeout_and_success(void)
{
    boot("OldAP");
    g_script[0] = WIFI_REASON_NO_AP_FOUND;
    g_script_len = 1;
    CHECK(wifi_manager_connect("GoneAP", "pw") == ESP_ERR_TIMEOUT);
    CHECK(s_wifi.connect_requested && g_timer_armed);   /* still owned */

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
    /* 200 went through the retry path (attempt 1, inline); that retry was then
     * silent, so the join's timeout handed the request to the timer. */
    CHECK(s_wifi.reconnect_count == 2);
    CHECK(g_timer_armed && g_timer_delay_us == 1000ULL * 1000ULL);
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

/* -- 2026-09 review round: no-event and concurrent paths ----------------- */

/* Fully silent join: disconnect and connect both return ESP_OK and NO event
 * ever arrives. Used to return TIMEOUT with the reconfigure latch armed and no
 * retry owner. */
static void scenario_join_silent_timeout_retries(void)
{
    boot("BenchAP");                                 /* unassociated */
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_TIMEOUT);
    CHECK(!s_wifi.reconfigure_in_progress);          /* latch disarmed */
    CHECK(s_wifi.connect_requested);
    CHECK(g_timer_armed && g_timer_delay_us == 1000ULL * 1000ULL);
    CHECK(s_wifi.reconnect_count == 1);
    CHECK(!failed_bit());
    CHECK(log_count('E', "no connect result within the wait") == 1);

    /* The disarmed latch no longer eats a real ASSOC_LEAVE. */
    ev_disconnect(WIFI_REASON_ASSOC_LEAVE);
    CHECK(log_count(0, "disconnected for reconfigure") == 0);
    CHECK(s_wifi.reconnect_count == 2);
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(fire_timer() == 500U);
    pump_pending();
    CHECK(wifi_manager_is_connected());
}

static esp_err_t g_hook_join_result;
static void hook_join_labap(void)
{
    g_hook_join_result = wifi_manager_connect("LabAP", "right-pass");
}

/* Deterministic interleaving: an OLD retry callback is blocked inside
 * esp_wifi_connect() while a fresh wifi_join runs; the old call then returns an
 * error. It must not touch the new request, and the join must not take the
 * attempt slot while that call is unresolved: after its own kick it opens a
 * recovery epoch (stop/start), whose connect for the join is refused by the
 * still-busy driver and retried on the join's own budget. */
static void scenario_stale_callback_vs_new_join(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* old request: 2 s retry */
    CHECK(g_timer_armed);

    g_connect_hook = hook_join_labap;
    g_hook_outer_err = ESP_ERR_WIFI_STATE;           /* old call fails AFTER the join */
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    (void)fire_timer();

    CHECK(g_hook_join_result != WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(g_stop_calls == 1 && log_count('W', "recovery epoch 1 complete") == 1);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(s_wifi.connect_requested);                 /* the join is still owned */
    CHECK(log_count('W', "stale retry result") == 1);

    CHECK(fire_timer() >= 1000U);                    /* the join's own retry */
    pump_pending();
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
}

static void hook_stored_reentry(void)
{
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* defers behind the call */
}

/* The OLD request's retry call errors AFTER a newer request (stored re-entry)
 * started and is still active, deferring behind that very call: the old error
 * must not be charged to the newer request's budget or FAILED bit. */
static void scenario_stale_callback_vs_active_reentry(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* old request: 2 s retry */
    g_connect_hook = hook_stored_reentry;
    g_hook_outer_err = ESP_ERR_WIFI_STATE;           /* old call fails AFTER the re-entry */
    (void)fire_timer();

    CHECK(s_wifi.connect_requested);                 /* the newer request is active */
    CHECK(s_wifi.reconnect_count == 1);              /* only its own deferral */
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(log_count('W', "stale retry result") == 1);
}

/* A callback already DISPATCHED for the old request when the join's
 * esp_timer_stop() ran (stop cannot recall it) must do nothing. */
static void scenario_dispatched_callback_after_new_join(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(g_timer_armed);
    expire_timer();                                  /* dispatch committed */
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(!g_timer_armed);
    CHECK(s_wifi.stale_dispatches == 1);
    const int calls = g_connect_calls;
    g_timer_cb(NULL);                                /* the late old callback */
    CHECK(g_connect_calls == calls);                 /* no connect on a live link */
    CHECK(wifi_manager_is_connected());
    CHECK(!failed_bit());
    CHECK(!g_timer_armed);
}

/* No timer can ever be created: an explicit terminal state, never
 * connect_requested=true with nothing owning the retry, and no inline
 * hammering in its place. */
static void scenario_timer_create_failure_terminal(void)
{
    g_timer_create_fail = -1;
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* needs a 2 s timer */
    CHECK(!s_wifi.connect_requested);
    CHECK(failed_bit());
    CHECK(g_connect_calls == 1);
    CHECK(log_count('E', "reconnect timer unavailable") == 1);

    /* Immediate attempts need no timer; the first delayed one ends it. */
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);          /* attempt 1: inline */
    CHECK(g_connect_calls == 3);
    CHECK(s_wifi.connect_requested);
    ev_disconnect(WIFI_REASON_NO_AP_FOUND);          /* attempt 2: 500 ms */
    CHECK(!s_wifi.connect_requested);
    CHECK(g_connect_calls == 3);
}

/* A create failure at init is retried lazily when a retry is first needed. */
static void scenario_timer_lazy_create_recovers(void)
{
    g_timer_create_fail = 1;
    boot("BenchAP");
    CHECK(g_timer_cb == NULL);
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(g_timer_cb != NULL);
    CHECK(g_timer_armed && g_timer_delay_us == 2000ULL * 1000ULL);
    CHECK(s_wifi.connect_requested);
}

/* esp_timer_start_once() failing: on the event path and on the no-event
 * (connect-error) path, the request ends explicitly. */
static void scenario_timer_start_failure_terminal(void)
{
    boot("BenchAP");
    g_timer_start_fail = true;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(!s_wifi.connect_requested && failed_bit() && !g_timer_armed);

    g_connect_err[0] = ESP_ERR_WIFI_STATE;           /* boot call fails, no event */
    g_connect_err_len = 1;
    g_connect_err_pos = 0;
    CHECK(wifi_manager_connect_stored_async() == ESP_ERR_WIFI_STATE);  /* nothing retries */
    CHECK(!s_wifi.connect_requested && failed_bit() && !g_timer_armed);
    CHECK(log_count('E', "reconnect timer unavailable") == 2);
}

/* Boot path: a transient error from the FIRST esp_wifi_connect() is handed to
 * the bounded retry instead of stranding the boot; config errors are not. */
static void scenario_boot_initial_connect_error_recovers(void)
{
    boot("BenchAP");
    g_connect_err[0] = ESP_ERR_WIFI_STATE;
    g_connect_err_len = 1;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* retry owned */
    CHECK(s_wifi.connect_requested);
    CHECK(g_timer_armed && g_timer_delay_us == 1000ULL * 1000ULL);
    CHECK(s_wifi.reconnect_count == 1);
    CHECK(log_count('E', "esp_wifi_connect (boot) failed") == 1);
    CHECK(fire_timer() == 1000U);
    pump_pending();
    CHECK(wifi_manager_is_connected());
    CHECK(s_wifi.reconnect_count == 0);
}

/* Boot with nothing stored: a configuration error ends the request and is
 * returned (nothing will retry). */
static void scenario_boot_config_error_no_retry(void)
{
    boot("");
    g_connect_err[0] = ESP_ERR_WIFI_SSID;
    g_connect_err_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_ERR_WIFI_SSID);
    CHECK(!s_wifi.connect_requested);
    CHECK(!g_timer_armed);
}

/* esp_wifi_set_config() -> ESP_ERR_WIFI_PASSWORD (malformed password) is a
 * driver validation error before anything is saved or attempted; it must not
 * read as the manager's "AP rejected, still retrying" result. */
static void scenario_set_config_password_error_distinct(void)
{
    boot("OldAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected());
    const int calls = g_connect_calls;

    g_set_config_err = ESP_ERR_WIFI_PASSWORD;
    const esp_err_t err = wifi_manager_connect("LabAP", "short");
    CHECK(err == ESP_ERR_WIFI_PASSWORD);
    CHECK(err != WIFI_MANAGER_ERR_AUTH_REJECTED);
    CHECK(g_connect_calls == calls);                 /* nothing attempted */
    CHECK(wifi_manager_is_connected());              /* old link untouched */
    CHECK(strcmp(s_wifi.current_ssid, "OldAP") == 0);
    CHECK(strcmp(wifi_manager_err_to_name(WIFI_MANAGER_ERR_AUTH_REJECTED),
                 "WIFI_MANAGER_ERR_AUTH_REJECTED") == 0);
}

/* -- review round 2: event attribution, dispatch identity, results ------- */

/* The OLD request's retry attempt returned ESP_OK; its outcome (AUTH_FAIL)
 * arrives only after a fresh join has started. It must not consume the new
 * request's budget, auth count, FAILED bit or timer. */
static void scenario_late_event_of_old_attempt_after_join(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* old request: 2 s retry */
    g_script[0] = WIFI_REASON_AUTH_FAIL;             /* the old retry's late outcome */
    g_script_len = 1;
    CHECK(fire_timer() == 2000U);                    /* old retry: connect OK */
    CHECK(g_pending_len == 1 && s_wifi.attempt_in_flight);

    const esp_err_t err = wifi_manager_connect("LabAP", "right-pass");   /* silent */
    CHECK(err == ESP_ERR_TIMEOUT);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(s_wifi.reconnect_count == 1);              /* only the join's own timeout */
    CHECK(!failed_bit());
    CHECK(g_timer_armed && g_timer_delay_us == 1000ULL * 1000ULL);
    CHECK(log_count('W', "late outcome (reason=202) of a superseded Wi-Fi attempt") == 1);
}

/* An OLD dispatch is committed (expired, callback not yet run) when a fresh
 * request arms its OWN timer on the same esp_timer. The old callback must not
 * pass as the new arm: no early retry, the new arm stays armed. */
static void scenario_old_dispatch_vs_new_arm(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    expire_timer();                                  /* old dispatch committed */
    CHECK(wifi_manager_connect("LabAP", "pw") == ESP_ERR_TIMEOUT);   /* arms 1 s */
    CHECK(g_timer_armed && s_wifi.timer_armed);
    CHECK(s_wifi.stale_dispatches == 1);

    const int calls = g_connect_calls;
    g_timer_cb(NULL);                                /* the old dispatch runs now */
    CHECK(g_connect_calls == calls);                 /* no early retry */
    CHECK(s_wifi.timer_armed && g_timer_armed);      /* new arm intact */
    CHECK(s_wifi.stale_dispatches == 0);
    const int kicks = g_disconnect_calls;
    CHECK(fire_timer() == 1000U);                    /* the new arm, on time */
    /* The join's own attempt is still silent: single-flight kicks it rather
     * than starting a second one. */
    CHECK(g_connect_calls == calls);
    CHECK(g_disconnect_calls == kicks + 1);
}

/* GOT_IP lands after the join's wait returned an empty snapshot: the join
 * reports the association and arms nothing. */
static void scenario_join_wait_races_got_ip(void)
{
    boot("BenchAP");
    g_wait_snapshot_first = true;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_OK);
    CHECK(wifi_manager_is_connected());
    CHECK(!g_timer_armed && !s_wifi.timer_armed);
    CHECK(s_wifi.reconnect_count == 0);
}

/* STA_CONNECTED (DHCP still pending) lands after the empty snapshot: the
 * request is progressing, so no retry is armed against the live station. */
static void scenario_join_wait_races_association_dhcp_pending(void)
{
    boot("BenchAP");
    g_wait_snapshot_first = true;
    g_script[0] = SCRIPT_CONNECTED_ONLY;
    g_script_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_TIMEOUT);
    CHECK(!g_timer_armed && !s_wifi.timer_armed);
    CHECK(s_wifi.connect_requested && s_wifi.associated);
    CHECK(!s_wifi.reconfigure_in_progress);
    ev_got_ip();                                     /* DHCP completes */
    CHECK(wifi_manager_is_connected());
}

/* Associated inside the wait but DHCP slower than it: same, without a race. */
static void scenario_join_associated_dhcp_slow(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_CONNECTED_ONLY;
    g_script_len = 1;
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_TIMEOUT);
    CHECK(!g_timer_armed && s_wifi.connect_requested);
}

/* Interactive auth rejection when no retry timer can be armed: the request is
 * ended, and the result says so instead of "still retrying". */
static void scenario_join_auth_reject_timer_failure(void)
{
    boot("BenchAP");
    g_timer_start_fail = true;
    g_script[0] = WIFI_REASON_AUTH_FAIL;
    g_script_len = 1;
    const esp_err_t err = wifi_manager_connect("BenchAP", "pw");
    CHECK(err == WIFI_MANAGER_ERR_NOT_RETRYING);
    CHECK(err != WIFI_MANAGER_ERR_AUTH_REJECTED);
    CHECK(!s_wifi.connect_requested && !g_timer_armed);
    CHECK(strcmp(wifi_manager_err_to_name(err), "WIFI_MANAGER_ERR_NOT_RETRYING") == 0);
}

/* Fully silent join when no retry timer can be armed: not ESP_ERR_TIMEOUT
 * (whose contract promises a retry) but the explicit terminal result. */
static void scenario_join_silent_timeout_timer_failure(void)
{
    boot("BenchAP");
    g_timer_start_fail = true;
    const esp_err_t err = wifi_manager_connect("BenchAP", "pw");
    CHECK(err == WIFI_MANAGER_ERR_NOT_RETRYING);
    CHECK(!s_wifi.connect_requested && !g_timer_armed);
    CHECK(!s_wifi.reconfigure_in_progress);
}

/* -- review round 3: single-flight attempt ownership ---------------------- */

/* The superseded attempt's outcome arrives AFTER the join's drain gave up.
 * The join's recovery epoch has stopped the station meanwhile, so that late
 * AUTH_FAIL belongs to the stopped epoch: it charges nobody, and the join
 * connects on the restarted station. */
static void hook_old_outcome_late(void)
{
    push_event(WIFI_REASON_AUTH_FAIL, 0, 0);         /* the old outcome, finally */
}

static void scenario_old_outcome_after_drain_timeout(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_defer_abort_event = true;                     /* its outcome comes late */
    g_stop_hook = hook_old_outcome_late;             /* ...after the drain gave up */
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(g_stop_calls == 1);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(log_count('W', "Wi-Fi disconnected (reason=202) during recovery epoch") == 1);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
}

/* A silent original attempt, then its retry kick, then a fresh join, then the
 * original's outcome. The slot is never overwritten or released by guess: the
 * join's recovery epoch ends the original by stopping the station, and the
 * original's outcome, delivered during the epoch, is charged to nobody. */
static void hook_late_original_auth_fail(void)
{
    push_event(WIFI_REASON_AUTH_FAIL, 0, 0);         /* queued ahead of STA_STOP */
}

static void scenario_silent_original_retry_then_join(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect("BenchAP", "pw") == ESP_ERR_TIMEOUT);   /* silent */
    const uint32_t old_gen = s_wifi.request_gen;
    CHECK(s_wifi.attempt_in_flight && g_timer_armed);
    g_defer_abort_event = true;
    const int calls = g_connect_calls;

    (void)fire_timer();                              /* the retry: kicks, defers */
    CHECK(g_connect_calls == calls);                 /* no second attempt */
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == old_gen);

    g_stop_hook = hook_late_original_auth_fail;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(log_count('W', "Wi-Fi disconnected (reason=202) during recovery epoch") == 1);
    CHECK(wifi_manager_link_is_current());
}

/* A superseded attempt's SUCCESS (STA_CONNECTED + GOT_IP) queued when a join
 * starts must not complete the join. */
static void scenario_stale_got_ip_during_join(void)
{
    boot("OldAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_driver_pending = false;                        /* ...which succeeded: */
    g_pending[g_pending_len++] = SCRIPT_GOT_IP;      /* outcome still queued */

    const esp_err_t err = wifi_manager_connect("LabAP", "right-pass");
    CHECK(err != ESP_OK);
    CHECK(!wifi_manager_is_connected());
    CHECK(log_count('W', "association from a superseded Wi-Fi attempt") == 1);
    CHECK(log_count('W', "IP on a superseded Wi-Fi link") == 1);
    CHECK(s_wifi.connect_requested);
}

/* A superseded attempt "associates" while a recovery epoch is stopping the
 * station: its STA_CONNECTED and GOT_IP belong to the old epoch, are not
 * credited and start no application services. The current request's own
 * association after the epoch gets the services. */
static void hook_stale_success(void)
{
    push_event(SCRIPT_STALE_CONNECTED, 0, 0);
    push_event(SCRIPT_GOT_IP_ONLY, 0, 0);
}

static void scenario_stale_connected_during_epoch_gated(void)
{
    boot("OldAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_defer_abort_event = true;
    g_stop_hook = hook_stale_success;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(log_count('W', "association during recovery epoch - old epoch, not credited") == 1);
    CHECK(g_app_link_refusals == 1);                 /* the stale GOT_IP */
    CHECK(g_app_link_starts == 1);                   /* only the current link */
    CHECK(s_wifi.link_gen == s_wifi.request_gen);
}

/* Re-entering the public stored-connect API while an attempt is unresolved
 * supersedes the request but never overwrites that attempt's slot. */
static void scenario_stored_reentry_does_not_overwrite(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    const uint32_t old_gen = s_wifi.request_gen;
    g_defer_abort_event = true;
    const int calls = g_connect_calls;

    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* re-entry */
    CHECK(s_wifi.request_gen != old_gen);
    CHECK(g_connect_calls == calls);
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == old_gen);
    CHECK(g_timer_armed);

    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* the first call's outcome */
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(!failed_bit());
    CHECK(log_count('W', "late outcome (reason=202) of a superseded Wi-Fi attempt") == 1);
}

/* The kick's outcome lands (and is dropped as stale) before start_attempt
 * re-takes the lock: the deferral must still leave a retry owner. */
static void scenario_kick_outcome_before_relock(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_disconnect_delivers_now = true;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* re-entry kicks it */
    CHECK(!s_wifi.attempt_in_flight);                /* stale outcome consumed */
    CHECK(log_count('W', "late outcome (reason=8) of a superseded Wi-Fi attempt") == 1);
    CHECK(g_timer_armed && s_wifi.connect_requested);   /* retry owned */
    const int calls = g_connect_calls;
    CHECK(fire_timer() == 1000U);
    CHECK(g_connect_calls == calls + 1);             /* now a clean single attempt */
}

/* -- review round 4: stale link and deferral truthfulness ----------------- */

/* A current link, then the request is superseded (stored re-entry): the old
 * link is torn down before any new connect, and a GOT_IP on it (DHCP renew)
 * starts no application services. */
static void scenario_superseded_link_gets_no_app_services(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected() && g_app_link_starts == 1);
    CHECK(wifi_manager_link_is_current());

    g_defer_abort_event = true;
    const int calls = g_connect_calls;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* supersedes the link */
    CHECK(g_connect_calls == calls);                 /* never connect over it */
    CHECK(!wifi_manager_link_is_current());
    CHECK(g_timer_armed);
    ev_got_ip();                                     /* late renew on the old link */
    CHECK(g_app_link_starts == 1 && g_app_link_refusals == 1);
}

/* Stored re-entry blocked by an unresolved attempt, and the deferral cannot
 * arm a retry: the request is ended and the call must say so. */
static void scenario_stored_reentry_deferral_timer_failure(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_defer_abort_event = true;
    g_timer_start_fail = true;
    CHECK(wifi_manager_connect_stored_async() == WIFI_MANAGER_ERR_NOT_RETRYING);
    CHECK(!s_wifi.connect_requested && !g_timer_armed);
}

/* Through the interactive path the join's own disconnect is the kick, so a
 * blocker that survives it ends the join truthfully (terminal either way, with
 * or without a timer). */
static void scenario_join_deferral_timer_failure(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_defer_abort_event = true;
    g_timer_start_fail = true;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(!s_wifi.connect_requested && !g_timer_armed);
}

/* -- hardware livelock, DEV 28:37:2F:FF:E7:04, 2026-09-29 ------------------ */

/* The exact bench sequence. Associated with MQTT up; `wifi_join` with a WRONG
 * passphrase: esp_wifi_set_config() drops the link inside the call and the
 * event task handles STA_DISCONNECTED reason=1 while the OLD generation is
 * still current; a connect issued in that window (on 43a2ec6: the old
 * request's immediate retry) is accepted and never answered. Then a CORRECT
 * `wifi_join`. On 43a2ec6 the old retry took the slot, every retry only kicked
 * and deferred, the wrong-pass attempt never ran and the correct join returned
 * ESP_ERR_TIMEOUT. With reason=1 owned by the reconfigure, no connect races
 * set_config at all. */
static void scenario_bench_wrongpass_join_then_correct_join(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected() && g_app_link_starts == 1);

    g_set_config_delivers_now = true;                /* reason=1 while gen is old */
    g_drop_connect_in_set_config = true;             /* the racing connect: no outcome */
    g_script[1] = WIFI_REASON_AUTH_FAIL;             /* what the wrong-pass attempt gets */
    g_script_len = 2;
    const esp_err_t wrong = wifi_manager_connect("BenchAP", "wrong-pass");
    CHECK(wrong == WIFI_MANAGER_ERR_AUTH_REJECTED);  /* the wrong password, reported */
    CHECK(!wifi_manager_is_connected());
    CHECK(s_wifi.connect_requested && g_timer_armed);   /* retrying on the auth floor */

    g_script[2] = SCRIPT_GOT_IP;
    g_script_len = 3;
    CHECK(wifi_manager_connect("BenchAP", "right-pass") == ESP_OK);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
    CHECK(g_app_link_starts == 2);

    /* How it got there: reason=1 belonged to the reconfigure, so the old
     * request issued no retry and nothing raced set_config. */
    CHECK(log_count('I', "disconnected for reconfigure (reason=1)") == 1);
    CHECK(log_count('W', "Wi-Fi disconnected (reason=1)") == 0);
}

/* The same ownership bug without the set_config path: the OLD request's
 * immediate retry is accepted and dropped by the driver, so it occupies the
 * slot with no outcome, ever. The join kicks it, opens a recovery epoch, and
 * connects on the restarted station. */
static void scenario_old_retry_dropped_join_recovers_via_epoch(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    const uint32_t old_gen = s_wifi.request_gen;

    g_connect_dropped = 1;
    ev_disconnect(WIFI_REASON_UNSPECIFIED);          /* old request: attempt 1 inline */
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == old_gen);   /* slot taken */

    g_script[1] = SCRIPT_GOT_IP;
    g_script_len = 2;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(g_stop_calls == 1 && g_start_calls == 2);  /* boot start + epoch restart */
    CHECK(log_count('W', "recovery epoch 1: stopping the station") == 1);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
}

/* A superseded link whose teardown kick posts no STA_DISCONNECTED: the next
 * start finds the same kicked link and ends it with a recovery epoch instead
 * of releasing it by guess, then connects the current request. */
static void scenario_stale_link_silent_kick_recovers_via_epoch(void)
{
    boot("OldAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected());

    g_disconnect_silent = true;                      /* teardown posts nothing */
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* supersedes, kicks */
    CHECK(s_wifi.associated && !wifi_manager_link_is_current() && g_timer_armed);
    g_script[1] = SCRIPT_GOT_IP;
    g_script_len = 2;
    (void)fire_timer();                              /* same kicked link: epoch */
    run_epoch();
    pump_pending();
    CHECK(g_stop_calls == 1);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
}

/* The kick's outcome is QUEUED but not yet delivered (busy event loop) when
 * the next retry runs: no connect is issued over the blocker; the recovery
 * epoch's barrier sits behind that queued outcome, which is then delivered
 * during the epoch and charged to nobody. */
static void scenario_queued_kick_outcome_gated_by_epoch(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* re-entry kicks it */
    CHECK(g_pending_len == 1);                       /* its outcome, not yet delivered */
    const int calls = g_connect_calls;
    (void)fire_timer();                              /* same blocker: epoch opens */
    CHECK(g_connect_calls == calls);                 /* nothing connected over it */
    CHECK(EPOCH_STATE == EPOCH_STOPPING);
    pump_pending();                                  /* old outcome, STA_STOP, barrier */
    CHECK(log_count('W', "Wi-Fi disconnected (reason=8) during recovery epoch") == 1);
    CHECK(log_count('W', "late outcome") == 0);
    CHECK(s_wifi.reconnect_count == 2);              /* kick deferral + epoch, no charge */
    run_epoch();
    CHECK(g_connect_calls == calls + 1);             /* the current request, once */
}

/* ROUND-6 HISTORICAL EVIDENCE. The Evaluator's round-6 regressions (patch
 * adversarial-probe-races.patch, sha256 9a24fbd1...) against the probe
 * handover of f206ef3, kept verbatim except for the _hist names. They assert
 * the probe's handover schedule, which the stop/start epoch replaces, so they
 * are not in k_scenarios / SCENARIOS; `--list-hist` names them and they can
 * still be run by name. Their invariants are covered under the epoch by
 * epoch_delayed_old_outcome_not_charged and epoch_stopped_request_not_resurrected.
 *
 * An old attempt outcome can land while the 2 s probe connect is inside the
 * driver. If that outcome exhausts the request budget, an ESP_OK probe must
 * not resurrect the stopped request or install a new in-flight slot. */
static void hook_old_auth_exhausts_request(void)
{
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
}

static void scenario_probe_success_cannot_resurrect_stopped_request_hist(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    const uint32_t gen = s_wifi.request_gen;
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);

    g_now_us += (int64_t)WIFI_MANAGER_KICK_OUTCOME_TIMEOUT_MS * 1000;
    s_wifi.reconnect_count = WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS;
    g_connect_hook = hook_old_auth_exhausts_request;
    g_hook_outer_err = ESP_OK;
    (void)wifi_manager_start_attempt(gen, &err);

    CHECK(!s_wifi.connect_requested);
    CHECK(!s_wifi.attempt_in_flight);
}

/* A kick outcome can be posted to the event loop but remain undelivered beyond
 * the 2 s heuristic. ESP_OK from the probe then installs the new slot before
 * the old outcome runs, so the old AUTH_FAIL must not be charged to the new
 * request. */
static void scenario_delayed_old_outcome_not_charged_to_probe_hist(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* request 1, silent */
    g_defer_abort_event = true;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* request 2 kicks request 1 */
    const uint32_t current_gen = s_wifi.request_gen;

    (void)fire_timer();                                    /* t=1 s, no probe */
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen != current_gen);

    /* Driver completed request 1 and queued its AUTH_FAIL, but the event-loop
     * task has not delivered it yet. The driver is idle for the t=2 s probe. */
    g_driver_pending = false;
    CHECK(g_pending_len < PENDING_MAX);
    g_pending[g_pending_len++] = WIFI_REASON_AUTH_FAIL;
    (void)fire_timer();                                    /* t=2 s probe: ESP_OK */
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == current_gen);
    pump_pending();                                        /* old result arrives */

    CHECK(s_wifi.auth_fail_count == 0);
}

/* -- round 7: independently written versions of the round-6 invariants ------ */

/* A superseded attempt has finished and its AUTH_FAIL is queued, undelivered.
 * The retry opens a recovery epoch; the queued outcome drains ahead of the
 * barrier and is charged to nobody; the current request then owns the fresh
 * attempt on the restarted station (the handover the round-6 test wanted,
 * made safe because stop ended the old attempt). */
static void scenario_own_delayed_old_outcome_never_charged(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* request 1, silent */
    g_defer_abort_event = true;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* request 2 kicks it */
    const uint32_t current_gen = s_wifi.request_gen;
    g_driver_pending = false;                        /* the driver finished request 1 */
    push_event(WIFI_REASON_AUTH_FAIL, 0, 0);         /* queued, not delivered */

    CHECK(g_timer_armed);
    (void)fire_timer();                              /* the retry: epoch */
    run_epoch();
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == current_gen);
    CHECK(log_count('W', "late outcome") == 0);
}

/* The blocker's own outcome lands while the manager's kick is inside the
 * driver (unlocked) and exhausts the request's budget. The request is ended and
 * must stay ended: no retry armed, no slot installed, not reported as pending. */
static void hook_own_attempt_auth_fail(void)
{
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
}

static void scenario_own_stopped_request_never_resurrected(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    const uint32_t gen = s_wifi.request_gen;
    s_wifi.reconnect_count = WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS;
    g_disconnect_hook = hook_own_attempt_auth_fail;
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_ENDED);
    CHECK(!s_wifi.connect_requested);
    CHECK(!s_wifi.attempt_in_flight);
    CHECK(!g_timer_armed && !s_wifi.timer_armed);
    CHECK(log_count('E', "gave up after 100 attempts") == 1);
}

/* -- STA stop/start recovery epoch -------------------------------------------- */

/* Open an epoch through the retry path: a silent own attempt, kicked once,
 * then retried while still unresolved. */
static void open_epoch_via_retry(void)
{
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    const uint32_t gen = s_wifi.request_gen;
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);  /* kick */
    CHECK(g_timer_armed);
    (void)fire_timer();                              /* same blocker: epoch */
}

static void check_epoch_failed_truthfully(void)
{
    CHECK(!s_wifi.connect_requested && failed_bit());
    CHECK(EPOCH_STATE == EPOCH_IDLE);
    CHECK(!g_epoch_timer_armed && !EPOCH_TIMER_ARMED_FLAG);
    CHECK(!g_timer_armed && !s_wifi.timer_armed);
    CHECK(REBOOT_NEEDED);
    CHECK(log_count('E', "recovery epoch 1 failed") == 1);
    /* A later request is refused truthfully, with no driver calls. */
    const int set_calls = g_set_config_calls, c = g_connect_calls, d = g_disconnect_calls;
    CHECK(wifi_manager_connect("LabAP", "pw") == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(wifi_manager_connect_stored_async() == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(g_set_config_calls == set_calls && g_connect_calls == c && g_disconnect_calls == d);
}

/* Model (b) of the bench: the next accepted connect is silent and kicks give
 * no event. On 9bfe64a the correct join ended DRIVER_UNRESOLVED; it must now
 * recover via stop/start and connect. */
static void scenario_bench_model_b_correct_join_recovers(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    CHECK(wifi_manager_is_connected() && g_app_link_starts == 1);

    g_set_config_delivers_now = true;
    g_connect_dropped = 1;                           /* the next accepted connect: silent */
    CHECK(wifi_manager_connect("BenchAP", "wrong-pass") == ESP_ERR_TIMEOUT);
    CHECK(log_count('W', "Wi-Fi disconnected (reason=1)") == 0);   /* reconfigure owned it */

    g_script[1] = SCRIPT_GOT_IP;
    g_script_len = 2;
    CHECK(wifi_manager_connect("BenchAP", "right-pass") == ESP_OK);
    CHECK(g_stop_calls == 1 && log_count('W', "recovery epoch 1 complete") == 1);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
    CHECK(g_app_link_starts == 2);
}

/* Model (b) with the retry phase: kicks return ESP_ERR_WIFI_STATE with no event;
 * the wrong-pass request's own attempt still runs (via the epoch) and is
 * rejected; then the correct join connects. */
static void scenario_bench_model_b_retry_phase_recovers(void)
{
    boot("BenchAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();

    g_set_config_delivers_now = true;
    g_connect_dropped = 1;
    g_script[1] = WIFI_REASON_AUTH_FAIL;             /* what the wrong-pass attempt gets */
    g_script_len = 2;
    CHECK(wifi_manager_connect("BenchAP", "wrong-pass") == ESP_ERR_TIMEOUT);
    g_kick_state_error = true;
    (void)fire_timer();                              /* kick: STATE, no event */
    CHECK(log_count('W', "(disconnect=ERR)") == 1);
    (void)fire_timer();                              /* same blocker: epoch */
    run_epoch();
    pump_pending();
    CHECK(s_wifi.auth_fail_count == 1);              /* the wrong-pass attempt RAN */
    g_kick_state_error = false;

    g_script[2] = SCRIPT_GOT_IP;
    g_script_len = 3;
    CHECK(wifi_manager_connect("BenchAP", "right-pass") == ESP_OK);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
}

/* Delayed old events queued BEFORE STA_STOP are handled during the epoch and
 * gain nothing. */
static void hook_old_events_before_stop(void)
{
    push_event(WIFI_REASON_AUTH_FAIL, 0, 0);
    push_event(SCRIPT_STALE_CONNECTED, 0, 0);
    push_event(SCRIPT_GOT_IP_ONLY, 0, 0);
}

static void scenario_epoch_old_events_before_stop_gated(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    g_defer_abort_event = true;
    g_stop_hook = hook_old_events_before_stop;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(log_count('W', "during recovery epoch") >= 2);
    CHECK(g_app_link_refusals == 1 && g_app_link_starts == 1);
}

/* ... queued BETWEEN STA_STOP and STA_START (behind the barrier) ... */
static void scenario_epoch_old_events_between_stop_and_start_gated(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    g_defer_abort_event = true;
    g_start_hook = hook_old_events_before_stop;      /* pushed ahead of STA_START */
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(g_app_link_refusals == 1 && g_app_link_starts == 1);
}

/* ... and an old-link GOT_IP (lwIP producer, no order against Wi-Fi events)
 * delivered AFTER STA_STOP, the barrier and STA_START but BEFORE the new
 * attempt's STA_CONNECTED: gated, no services. The new association then is. */
static void scenario_epoch_stale_got_ip_before_new_connected_gated(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    g_defer_abort_event = true;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_ERR_TIMEOUT);  /* new: silent */
    CHECK(EPOCH_STATE == EPOCH_IDLE);                  /* after START */
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == s_wifi.request_gen);
    ev_got_ip();                                     /* the stale old-link GOT_IP */
    CHECK(!wifi_manager_is_connected() && g_app_link_refusals == 1 && g_app_link_starts == 0);
    ev_connected_only();                             /* the new attempt's association */
    ev_got_ip();
    CHECK(wifi_manager_is_connected() && g_app_link_starts == 1);
}

/* Stopping while associated raises ASSOC_LEAVE before STA_STOP: it belongs to
 * the old epoch - no retry, no auth charge. */
static void scenario_epoch_stop_while_associated_assoc_leave_ignored(void)
{
    boot("OldAP");
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    pump_pending();
    g_disconnect_ignored = true;                    /* the kick leaves it associated */
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* supersedes the link */
    pump_pending();
    CHECK(g_timer_armed && s_wifi.associated);
    g_script[1] = SCRIPT_GOT_IP;
    g_script_len = 2;
    (void)fire_timer();                              /* epoch: stop while associated */
    run_epoch();
    pump_pending();
    CHECK(log_count('W', "Wi-Fi disconnected (reason=8) during recovery epoch") == 1);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(wifi_manager_is_connected() && wifi_manager_link_is_current());
}

/* Concurrent replacement requests mid-epoch: the superseded request is never
 * resurrected; the epoch connects whichever request is current at STA_START. */
static esp_err_t g_replacement_result;
static uint32_t g_replacement_gen;
static void hook_join_replacement(void)
{
    g_script[g_script_len++] = SCRIPT_GOT_IP;
    g_replacement_result = wifi_manager_connect("LabAP", "right-pass");
    g_replacement_gen = s_wifi.request_gen;          /* the replacement's own gen */
}

static void scenario_epoch_replacement_join_during_stop(void)
{
    boot("BenchAP");
    g_stop_hook = hook_join_replacement;
    open_epoch_via_retry();                          /* unlocked in stop: a join */
    CHECK(g_replacement_result == ESP_ERR_TIMEOUT);  /* owned by the epoch */
    const uint32_t join_gen = g_replacement_gen;
    CHECK(s_wifi.request_gen == join_gen);           /* the old one not resurrected */
    run_epoch();
    pump_pending();
    CHECK(s_wifi.request_gen == join_gen);
    CHECK(wifi_manager_is_connected() && s_wifi.link_gen == join_gen);
    CHECK(log_count('W', "recovery epoch 1 complete: station restarted; connecting") == 1);
}

static void scenario_epoch_replacement_join_between_stop_and_start(void)
{
    boot("BenchAP");
    open_epoch_via_retry();
    while (EPOCH_STATE == EPOCH_STOPPING) {
        CHECK(pump_one());                           /* STA_STOP, barrier */
    }
    CHECK(EPOCH_STATE == EPOCH_RESUMING);
    const uint32_t old_gen = s_wifi.request_gen;
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    CHECK(wifi_manager_connect("LabAP", "right-pass") == ESP_OK);
    CHECK(s_wifi.request_gen != old_gen && s_wifi.link_gen == s_wifi.request_gen);
}

static void hook_stored_reentry_in_start(void)
{
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
}

static void scenario_epoch_replacement_stored_during_start(void)
{
    boot("BenchAP");
    open_epoch_via_retry();
    const uint32_t old_gen = s_wifi.request_gen;
    g_start_hook = hook_stored_reentry_in_start;     /* unlocked in start: re-entry */
    g_script[0] = SCRIPT_GOT_IP;
    g_script_len = 1;
    const int calls = g_connect_calls;
    run_epoch();
    pump_pending();
    CHECK(s_wifi.request_gen != old_gen);
    CHECK(g_connect_calls == calls + 1);             /* one connect, for the current one */
    CHECK(wifi_manager_is_connected() && s_wifi.link_gen == s_wifi.request_gen);
}

/* The round-6 invariants under the epoch. */
static void scenario_epoch_delayed_old_outcome_not_charged(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* request 1, silent */
    g_defer_abort_event = true;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* request 2 kicks it */
    const uint32_t current_gen = s_wifi.request_gen;
    /* The driver completed request 1 and queued its AUTH_FAIL, but the
     * event-loop task has not delivered it (the round-6 shape). */
    g_driver_pending = false;
    push_event(WIFI_REASON_AUTH_FAIL, 0, 0);
    (void)fire_timer();                              /* the retry: recovery epoch */
    run_epoch();
    /* The new request owns the fresh attempt (safe: stop ended the old one)
     * and the old AUTH_FAIL, drained ahead of the barrier, charged nobody. */
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == current_gen);
    CHECK(s_wifi.auth_fail_count == 0);
    CHECK(log_count('W', "Wi-Fi disconnected (reason=202) during recovery epoch") == 1);
}

static void hook_old_auth_fail_in_stop(void)
{
    ev_stale_disconnect(WIFI_REASON_AUTH_FAIL);      /* lands while stop is unlocked */
}

static void scenario_epoch_stopped_request_not_resurrected(void)
{
    /* Exhausted at the epoch: ends, no stop at all. */
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    const uint32_t gen = s_wifi.request_gen;
    s_wifi.reconnect_count = WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS - 1;
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);
    s_wifi.timer_armed = false;
    g_timer_armed = false;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_ENDED);
    CHECK(!s_wifi.connect_requested && g_stop_calls == 0);
    CHECK(EPOCH_STATE == EPOCH_IDLE && !g_epoch_timer_armed);
    CHECK(!s_wifi.timer_armed);
}

/* An old outcome landing while esp_wifi_stop() is unlocked in the driver is
 * gated: it charges nothing, and the request is neither ended by it nor
 * resurrected; the epoch then connects it. */
static void scenario_epoch_old_outcome_in_unlocked_stop_gated(void)
{
    boot("BenchAP");
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    const uint32_t gen2 = s_wifi.request_gen;
    CHECK(wifi_manager_start_attempt(gen2, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);
    g_stop_hook = hook_old_auth_fail_in_stop;
    s_wifi.timer_armed = false;
    g_timer_armed = false;
    CHECK(wifi_manager_start_attempt(gen2, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);
    CHECK(s_wifi.auth_fail_count == 0 && s_wifi.connect_requested);
    run_epoch();
    CHECK(s_wifi.attempt_in_flight && s_wifi.attempt_gen == gen2);
}

/* Budget: the epoch's connect is this request's next attempt; nothing resets. */
static void scenario_epoch_budget_preserved(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    const uint32_t gen = s_wifi.request_gen;
    s_wifi.reconnect_count = 5;
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);  /* 6 */
    g_script[0] = WIFI_REASON_AUTH_FAIL;
    g_script_len = 1;
    (void)fire_timer();                              /* epoch: 7 */
    run_epoch();
    pump_pending();                                  /* AUTH_FAIL: 8 */
    CHECK(s_wifi.reconnect_count == 8);
    CHECK(s_wifi.connect_requested && g_timer_armed);
}

static void scenario_epoch_budget_exhaustion_terminal(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);
    const uint32_t gen = s_wifi.request_gen;
    s_wifi.reconnect_count = WIFI_MANAGER_RECONNECT_MAX_ATTEMPTS - 2;
    esp_err_t err = ESP_OK;
    CHECK(wifi_manager_start_attempt(gen, &err) == WIFI_MANAGER_ATTEMPT_DEFERRED);
    g_script[0] = WIFI_REASON_AUTH_FAIL;
    g_script_len = 1;
    (void)fire_timer();                              /* epoch takes the last attempt */
    run_epoch();
    pump_pending();                                  /* its failure exhausts the budget */
    CHECK(!s_wifi.connect_requested && log_count('E', "gave up after 100 attempts") == 1);
    CHECK(!g_timer_armed && !g_epoch_timer_armed);
}

/* Races: STA_STOP handled before stop() returns; barrier handled before the
 * poster's bookkeeping; STA_STOP arriving after the barrier. All recover. */
static void scenario_epoch_sta_stop_before_stop_returns(void)
{
    boot("BenchAP");
    g_stop_delivers_now = true;
    open_epoch_via_retry();
    g_script[0] = SCRIPT_GOT_IP;                     /* for the epoch's connect */
    g_script_len = 1;
    run_epoch();
    pump_pending();
    CHECK(wifi_manager_is_connected());
}

static void scenario_epoch_barrier_before_bookkeeping(void)
{
    boot("BenchAP");
    g_barrier_delivers_now = true;
    open_epoch_via_retry();
    g_script[0] = SCRIPT_GOT_IP;                     /* for the epoch's connect */
    g_script_len = 1;
    CHECK(EPOCH_STATE == EPOCH_RESUMING);
    run_epoch();
    pump_pending();
    CHECK(wifi_manager_is_connected());
}

static void scenario_epoch_sta_stop_after_barrier(void)
{
    boot("BenchAP");
    g_sta_stop_after_barrier = true;
    open_epoch_via_retry();
    g_script[0] = SCRIPT_GOT_IP;                     /* for the epoch's connect */
    g_script_len = 1;
    pump_pending();                                  /* fence, STA_STOP, STOP_DONE */
    CHECK(g_barrier_posts == 2);                     /* the second barrier was needed */
    run_epoch();
    pump_pending();
    CHECK(wifi_manager_is_connected());
}

/* The public stored-connect API opens the epoch itself (second re-entry on the
 * same kicked blocker) and esp_wifi_stop() fails: it must report the terminal
 * truthfully, never ESP_OK. */
static void scenario_epoch_stop_error_via_stored_reentry(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* kicks it, defers */
    g_stop_err = ESP_FAIL;
    CHECK(wifi_manager_connect_stored_async() == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(!s_wifi.connect_requested && REBOOT_NEEDED);
}

/* Failed recoveries: each ends DRIVER_UNRESOLVED with nothing leaked. */
static void scenario_epoch_stop_error_terminal(void)
{
    boot("BenchAP");
    g_stop_err = ESP_FAIL;
    open_epoch_via_retry();
    check_epoch_failed_truthfully();
}

static void scenario_epoch_barrier_post_failure_terminal(void)
{
    boot("BenchAP");
    g_barrier_post_err = ESP_FAIL;
    open_epoch_via_retry();
    check_epoch_failed_truthfully();
}

static void scenario_epoch_missing_sta_stop_deadline(void)
{
    boot("BenchAP");
    g_lose_sta_stop = true;
    open_epoch_via_retry();
    pump_pending();                                  /* only the barrier arrives */
    CHECK(EPOCH_STATE == EPOCH_STOPPING && g_epoch_timer_armed);
    fire_epoch_timer();                              /* the deadline */
    check_epoch_failed_truthfully();
}

static void scenario_epoch_missing_barrier_deadline(void)
{
    boot("BenchAP");
    g_lose_barrier = true;
    open_epoch_via_retry();
    pump_pending();                                  /* only STA_STOP arrives */
    CHECK(EPOCH_STATE == EPOCH_STOPPING && g_epoch_timer_armed);
    fire_epoch_timer();
    check_epoch_failed_truthfully();
}

static void scenario_epoch_start_error_terminal(void)
{
    boot("BenchAP");
    g_start_err = ESP_FAIL;
    open_epoch_via_retry();
    run_epoch();
    check_epoch_failed_truthfully();
}

static void scenario_epoch_missing_sta_start_deadline(void)
{
    boot("BenchAP");
    g_lose_sta_start = true;
    open_epoch_via_retry();
    run_epoch();
    CHECK(EPOCH_STATE == EPOCH_STARTING && g_epoch_timer_armed);
    fire_epoch_timer();
    check_epoch_failed_truthfully();
}

/* -- round 8: nothing may credit or resurrect after a failed recovery epoch --- */

typedef struct {
    int reconnect_count, auth_fail_count;
    bool reconfigure, connect_requested;
    int connect_calls, disconnect_calls, stop_calls, start_calls, set_config_calls;
    int barrier_posts, app_starts;
    uint32_t request_gen;
    int log_assoc, log_ip, log_202, log_8;
} late_snapshot_t;

#define LATE_LOG_ASSOC "association after a failed recovery epoch - reboot required"
#define LATE_LOG_IP "IP after a failed recovery epoch - reboot required"
#define LATE_LOG_202 "(reason=202) after a failed recovery epoch - reboot required"
#define LATE_LOG_8 "(reason=8) after a failed recovery epoch - reboot required"

static late_snapshot_t late_snapshot(void)
{
    late_snapshot_t b;
    b.reconnect_count = s_wifi.reconnect_count;
    b.auth_fail_count = s_wifi.auth_fail_count;
    b.reconfigure = s_wifi.reconfigure_in_progress;
    b.connect_requested = s_wifi.connect_requested;
    b.connect_calls = g_connect_calls;
    b.disconnect_calls = g_disconnect_calls;
    b.stop_calls = g_stop_calls;
    b.start_calls = g_start_calls;
    b.set_config_calls = g_set_config_calls;
    b.barrier_posts = g_barrier_posts;
    b.app_starts = g_app_link_starts;
    b.request_gen = s_wifi.request_gen;
    b.log_assoc = log_count('W', LATE_LOG_ASSOC);
    b.log_ip = log_count('W', LATE_LOG_IP);
    b.log_202 = log_count('W', LATE_LOG_202);
    b.log_8 = log_count('W', LATE_LOG_8);
    return b;
}

/* Late events of the failed epoch, in the orders the review named. */
static void deliver_late_events(void)
{
    ev_connected_only();                             /* STA_CONNECTED ...          */
    ev_got_ip();                                     /* ... then GOT_IP (+ app)    */
    ev_disconnect(WIFI_REASON_AUTH_FAIL);            /* STA_DISCONNECTED 202       */
    ev_disconnect(WIFI_REASON_ASSOC_LEAVE);          /* STA_DISCONNECTED 8         */
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    g_wifi_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    ev_connected_only();                             /* and once more after those  */
    ev_got_ip();
}

static void check_late_events_ignored(const late_snapshot_t *b)
{
    CHECK(!wifi_manager_link_is_current());          /* no app service start */
    CHECK(!wifi_manager_is_connected());             /* no CONNECTED bit */
    CHECK(g_app_link_starts == b->app_starts);
    CHECK(!s_wifi.associated);                       /* no ownership */
    CHECK(!s_wifi.attempt_in_flight);                /* no attempt credit */
    CHECK(s_wifi.reconnect_count == b->reconnect_count);   /* no retry-state reset */
    CHECK(s_wifi.auth_fail_count == b->auth_fail_count);
    CHECK(s_wifi.reconfigure_in_progress == b->reconfigure);
    CHECK(!s_wifi.connect_requested && !b->connect_requested);   /* not resurrected */
    CHECK(failed_bit());
    CHECK(!g_timer_armed && !s_wifi.timer_armed);
    CHECK(!g_epoch_timer_armed && !EPOCH_TIMER_ARMED_FLAG);
    CHECK(EPOCH_STATE == EPOCH_IDLE);
    CHECK(g_connect_calls == b->connect_calls && g_disconnect_calls == b->disconnect_calls);
    CHECK(g_stop_calls == b->stop_calls && g_start_calls == b->start_calls);
    CHECK(g_set_config_calls == b->set_config_calls && g_barrier_posts == b->barrier_posts);
    CHECK(s_wifi.request_gen == b->request_gen);
    CHECK(REBOOT_NEEDED);
    CHECK(log_count('W', LATE_LOG_ASSOC) == b->log_assoc + 2);   /* each one logged */
    CHECK(log_count('W', LATE_LOG_IP) == b->log_ip + 2);
    CHECK(log_count('W', LATE_LOG_202) == b->log_202 + 1);
    CHECK(log_count('W', LATE_LOG_8) == b->log_8 + 1);
    /* Later requests are still refused, with no driver calls. */
    CHECK(wifi_manager_connect("LabAP", "pw") == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(wifi_manager_connect_stored_async() == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(g_set_config_calls == b->set_config_calls && g_connect_calls == b->connect_calls);
    CHECK(g_disconnect_calls == b->disconnect_calls);
}

static void late_events_after_failure(void)
{
    const late_snapshot_t b = late_snapshot();
    deliver_late_events();
    check_late_events_ignored(&b);
}

static void scenario_late_events_after_stop_error_via_retry(void)
{
    boot("BenchAP");
    g_stop_err = ESP_FAIL;
    open_epoch_via_retry();
    check_epoch_failed_truthfully();
    late_events_after_failure();
}

static void scenario_late_events_after_stop_error_via_stored_reentry(void)
{
    boot("BenchAP");
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* silent attempt */
    CHECK(wifi_manager_connect_stored_async() == ESP_OK);   /* kicks it, defers */
    g_stop_err = ESP_FAIL;
    CHECK(wifi_manager_connect_stored_async() == WIFI_MANAGER_ERR_DRIVER_UNRESOLVED);
    CHECK(!s_wifi.connect_requested && REBOOT_NEEDED);
    late_events_after_failure();
}

static void scenario_late_events_after_barrier_post_failure(void)
{
    boot("BenchAP");
    g_barrier_post_err = ESP_FAIL;
    open_epoch_via_retry();
    check_epoch_failed_truthfully();
    pump_pending();                                  /* the driver's own STA_STOP */
    late_events_after_failure();
}

static void scenario_late_events_after_missing_sta_stop_deadline(void)
{
    boot("BenchAP");
    g_lose_sta_stop = true;
    open_epoch_via_retry();
    pump_pending();
    fire_epoch_timer();
    check_epoch_failed_truthfully();
    late_events_after_failure();
}

static void scenario_late_events_after_missing_barrier_deadline(void)
{
    boot("BenchAP");
    g_lose_barrier = true;
    open_epoch_via_retry();
    pump_pending();
    fire_epoch_timer();
    check_epoch_failed_truthfully();
    late_events_after_failure();
}

static void scenario_late_events_after_start_error(void)
{
    boot("BenchAP");
    g_start_err = ESP_FAIL;
    open_epoch_via_retry();
    run_epoch();
    check_epoch_failed_truthfully();
    late_events_after_failure();
}

static void scenario_late_events_after_missing_sta_start_deadline(void)
{
    boot("BenchAP");
    g_lose_sta_start = true;
    open_epoch_via_retry();
    run_epoch();
    fire_epoch_timer();
    check_epoch_failed_truthfully();
    late_events_after_failure();
}

/* The round-7 reproduction, exactly: a late STA_CONNECTED then GOT_IP after a
 * stop-error epoch failure made the link current, set CONNECTED and started app
 * services while the request was DRIVER_UNRESOLVED. */
static void scenario_zz_p1_late_connected_after_failed_epoch(void)
{
    boot("BenchAP");
    g_stop_err = ESP_FAIL;
    open_epoch_via_retry();
    check_epoch_failed_truthfully();
    const int before = g_app_link_starts;
    ev_connected_only();
    ev_got_ip();
    CHECK(!wifi_manager_link_is_current() && !wifi_manager_is_connected() &&
          g_app_link_starts == before);
    CHECK(REBOOT_NEEDED);
}

/* Defence in depth: each gate must hold ON ITS OWN. After the latch, the state
 * each gate protects is FORCED (as if some other path had leaked it), so that
 * removing any single gate is observable. In the natural flow the STA_CONNECTED
 * gate already keeps associated false and connect_requested stays false. */
static void scenario_failed_epoch_gates_hold_even_if_state_leaks(void)
{
    boot("BenchAP");
    g_stop_err = ESP_FAIL;
    open_epoch_via_retry();
    check_epoch_failed_truthfully();
    const int starts = g_app_link_starts;
    const int calls = g_connect_calls;

    /* link_is_current() and GOT_IP, with ownership leaked. */
    s_wifi.associated = true;
    s_wifi.link_gen = s_wifi.request_gen;
    s_wifi.reconnect_count = 7;
    s_wifi.auth_fail_count = 3;
    CHECK(!wifi_manager_link_is_current());
    ev_got_ip();
    CHECK(!wifi_manager_is_connected() && g_app_link_starts == starts);
    CHECK(s_wifi.reconnect_count == 7 && s_wifi.auth_fail_count == 3);
    s_wifi.associated = false;

    /* STA_CONNECTED: no ownership, FAILED kept. */
    ev_connected_only();
    CHECK(!s_wifi.associated && failed_bit());

    /* STA_DISCONNECTED and the reconnect timer, with the request leaked. */
    s_wifi.connect_requested = true;
    ev_disconnect(WIFI_REASON_AUTH_FAIL);
    CHECK(!g_timer_armed && s_wifi.reconnect_count == 7 && s_wifi.auth_fail_count == 3);
    g_timer_cb(NULL);                                /* a late dispatch */
    CHECK(g_connect_calls == calls && !s_wifi.attempt_in_flight);
    CHECK(log_count('W', "reconnect timer after a failed recovery epoch") == 1);
    s_wifi.connect_requested = false;
    CHECK(REBOOT_NEEDED);
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
    static const struct { const char *name; void (*fn)(void); } k_scenarios[] = {
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
        { "join_silent_timeout_retries", scenario_join_silent_timeout_retries },
        { "stale_callback_vs_new_join", scenario_stale_callback_vs_new_join },
        { "dispatched_callback_after_new_join", scenario_dispatched_callback_after_new_join },
        { "timer_create_failure_terminal", scenario_timer_create_failure_terminal },
        { "timer_lazy_create_recovers", scenario_timer_lazy_create_recovers },
        { "timer_start_failure_terminal", scenario_timer_start_failure_terminal },
        { "boot_initial_connect_error_recovers", scenario_boot_initial_connect_error_recovers },
        { "set_config_password_error_distinct", scenario_set_config_password_error_distinct },
        { "late_event_of_old_attempt_after_join", scenario_late_event_of_old_attempt_after_join },
        { "old_dispatch_vs_new_arm", scenario_old_dispatch_vs_new_arm },
        { "join_wait_races_got_ip", scenario_join_wait_races_got_ip },
        { "join_wait_races_association_dhcp_pending",
          scenario_join_wait_races_association_dhcp_pending },
        { "join_associated_dhcp_slow", scenario_join_associated_dhcp_slow },
        { "join_auth_reject_timer_failure", scenario_join_auth_reject_timer_failure },
        { "join_silent_timeout_timer_failure", scenario_join_silent_timeout_timer_failure },
        { "old_outcome_after_drain_timeout", scenario_old_outcome_after_drain_timeout },
        { "silent_original_retry_then_join", scenario_silent_original_retry_then_join },
        { "stale_got_ip_during_join", scenario_stale_got_ip_during_join },
        { "stale_connected_during_epoch_gated", scenario_stale_connected_during_epoch_gated },
        { "stored_reentry_does_not_overwrite", scenario_stored_reentry_does_not_overwrite },
        { "kick_outcome_before_relock", scenario_kick_outcome_before_relock },
        { "boot_config_error_no_retry", scenario_boot_config_error_no_retry },
        { "superseded_link_gets_no_app_services", scenario_superseded_link_gets_no_app_services },
        { "stored_reentry_deferral_timer_failure", scenario_stored_reentry_deferral_timer_failure },
        { "join_deferral_timer_failure", scenario_join_deferral_timer_failure },
        { "bench_wrongpass_join_then_correct_join",
          scenario_bench_wrongpass_join_then_correct_join },
        { "old_retry_dropped_join_recovers_via_epoch",
          scenario_old_retry_dropped_join_recovers_via_epoch },
        { "stale_link_silent_kick_recovers_via_epoch",
          scenario_stale_link_silent_kick_recovers_via_epoch },
        { "queued_kick_outcome_gated_by_epoch", scenario_queued_kick_outcome_gated_by_epoch },

        { "own_delayed_old_outcome_never_charged", scenario_own_delayed_old_outcome_never_charged },
        { "own_stopped_request_never_resurrected", scenario_own_stopped_request_never_resurrected },
        { "stale_callback_vs_active_reentry", scenario_stale_callback_vs_active_reentry },
        { "bench_model_b_correct_join_recovers", scenario_bench_model_b_correct_join_recovers },
        { "bench_model_b_retry_phase_recovers", scenario_bench_model_b_retry_phase_recovers },
        { "epoch_old_events_before_stop_gated", scenario_epoch_old_events_before_stop_gated },
        { "epoch_old_events_between_stop_and_start_gated",
          scenario_epoch_old_events_between_stop_and_start_gated },
        { "epoch_stale_got_ip_before_new_connected_gated",
          scenario_epoch_stale_got_ip_before_new_connected_gated },
        { "epoch_stop_while_associated_assoc_leave_ignored",
          scenario_epoch_stop_while_associated_assoc_leave_ignored },
        { "epoch_replacement_join_during_stop", scenario_epoch_replacement_join_during_stop },
        { "epoch_replacement_join_between_stop_and_start",
          scenario_epoch_replacement_join_between_stop_and_start },
        { "epoch_replacement_stored_during_start", scenario_epoch_replacement_stored_during_start },
        { "epoch_delayed_old_outcome_not_charged", scenario_epoch_delayed_old_outcome_not_charged },
        { "epoch_stopped_request_not_resurrected", scenario_epoch_stopped_request_not_resurrected },
        { "epoch_old_outcome_in_unlocked_stop_gated",
          scenario_epoch_old_outcome_in_unlocked_stop_gated },
        { "epoch_budget_preserved", scenario_epoch_budget_preserved },
        { "epoch_budget_exhaustion_terminal", scenario_epoch_budget_exhaustion_terminal },
        { "epoch_sta_stop_before_stop_returns", scenario_epoch_sta_stop_before_stop_returns },
        { "epoch_barrier_before_bookkeeping", scenario_epoch_barrier_before_bookkeeping },
        { "epoch_sta_stop_after_barrier", scenario_epoch_sta_stop_after_barrier },
        { "epoch_stop_error_terminal", scenario_epoch_stop_error_terminal },
        { "epoch_stop_error_via_stored_reentry", scenario_epoch_stop_error_via_stored_reentry },
        { "epoch_barrier_post_failure_terminal", scenario_epoch_barrier_post_failure_terminal },
        { "epoch_missing_sta_stop_deadline", scenario_epoch_missing_sta_stop_deadline },
        { "epoch_missing_barrier_deadline", scenario_epoch_missing_barrier_deadline },
        { "epoch_start_error_terminal", scenario_epoch_start_error_terminal },
        { "epoch_missing_sta_start_deadline", scenario_epoch_missing_sta_start_deadline },
        { "late_events_after_stop_error_via_retry",
          scenario_late_events_after_stop_error_via_retry },
        { "late_events_after_stop_error_via_stored_reentry",
          scenario_late_events_after_stop_error_via_stored_reentry },
        { "late_events_after_barrier_post_failure",
          scenario_late_events_after_barrier_post_failure },
        { "late_events_after_missing_sta_stop_deadline",
          scenario_late_events_after_missing_sta_stop_deadline },
        { "late_events_after_missing_barrier_deadline",
          scenario_late_events_after_missing_barrier_deadline },
        { "late_events_after_start_error", scenario_late_events_after_start_error },
        { "late_events_after_missing_sta_start_deadline",
          scenario_late_events_after_missing_sta_start_deadline },
        { "zz_p1_late_connected_after_failed_epoch",
          scenario_zz_p1_late_connected_after_failed_epoch },
        { "failed_epoch_gates_hold_even_if_state_leaks",
          scenario_failed_epoch_gates_hold_even_if_state_leaks },
    };
    /* Round-6 historical evidence (see the _hist functions): not in SCENARIOS. */
    static const struct { const char *name; void (*fn)(void); } k_round6_hist[] = {
        { "probe_success_cannot_resurrect_stopped_request_hist",
          scenario_probe_success_cannot_resurrect_stopped_request_hist },
        { "delayed_old_outcome_not_charged_to_probe_hist",
          scenario_delayed_old_outcome_not_charged_to_probe_hist },
    };
    const size_t n_hist = sizeof(k_round6_hist) / sizeof(k_round6_hist[0]);
    const size_t n_scenarios = sizeof(k_scenarios) / sizeof(k_scenarios[0]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scenario|--list>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--list-hist") == 0) {
        for (size_t i = 0; i < n_hist; i++) {
            printf("%s\n", k_round6_hist[i].name);
        }
        return 0;
    }
    for (size_t i = 0; i < n_hist; i++) {
        if (strcmp(argv[1], k_round6_hist[i].name) == 0) {
            k_round6_hist[i].fn();
            printf("PASS %s\n", k_round6_hist[i].name);
            return 0;
        }
    }
    if (strcmp(argv[1], "--list") == 0) {   /* lets pytest detect registry drift */
        for (size_t i = 0; i < n_scenarios; i++) {
            printf("%s\n", k_scenarios[i].name);
        }
        return 0;
    }
    for (size_t i = 0; i < n_scenarios; i++) {
        if (strcmp(argv[1], k_scenarios[i].name) == 0) {
            k_scenarios[i].fn();
            printf("PASS %s\n", k_scenarios[i].name);
            return 0;
        }
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}

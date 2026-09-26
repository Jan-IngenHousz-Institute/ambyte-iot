/*
 * esp_stubs.c — header-level stubs for the evq integration harness.
 *
 * API surface only: nothing here makes a publish, delivery, spool or cursor
 * decision. NVS is a key/value map persisted on nvs_commit (so a forked
 * "reboot" sees committed state only); esp_littlefs_info reports the configured
 * capacity and the block-rounded usage of EVSTORE_MOUNT; sd_card.h is driven
 * by h_media (mount flag, CID, loss latch, capacity) and asserts the
 * sdcard_io_begin/end bracket is 1:1. The media wrappers (prelude.h) forward to
 * libc and only add capacity/EIO/trace behaviour.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_pm.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#include "miniz.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sd_card.h"
#include "clock_trust.h"
#include "fleet_jitter.h"
#include "ota_update.h"
#include "timezone.h"
#include "wifi_manager.h"
#include "shim_api.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef EVSTORE_MOUNT
#error "EVSTORE_MOUNT must be defined for the harness"
#endif

h_media_cfg_t h_media = {
    .flash_total = 1048576,
    .sd_total = 256ULL * 1024 * 1024,
    .sd_mounted = false,
    .sd_cid = 0,
    .heap_internal_largest = 1024 * 1024,
    .app_version = "0.0.0-evq-integ",
};
h_media_stats_t h_mstats;

static void fatal(const char *msg)
{
    fprintf(stderr, "[stubs] FATAL: %s\n", msg);
    abort();
}

/* ── esp_err ── */
const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_NOT_FINISHED: return "ESP_ERR_NOT_FINISHED";
    case ESP_ERR_NOT_ALLOWED: return "ESP_ERR_NOT_ALLOWED";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NOT_INITIALIZED: return "ESP_ERR_NVS_NOT_INITIALIZED";
    case ESP_ERR_NVS_TYPE_MISMATCH: return "ESP_ERR_NVS_TYPE_MISMATCH";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    default: return "ESP_ERR_UNKNOWN";
    }
}

/* ── media usage ── */

uint64_t h_dir_used_bytes(const char *dir)
{
    uint64_t used = 4096;                       /* the directory itself */
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) used += h_dir_used_bytes(p);
        else used += ((uint64_t)st.st_size + 4095U) / 4096U * 4096U;
    }
    closedir(d);
    return used;
}

esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    if (label == NULL || strcmp(label, "storage") != 0) return ESP_ERR_NOT_FOUND;
    if (total) *total = h_media.flash_total;
    if (used)  *used = (size_t)h_dir_used_bytes(EVSTORE_MOUNT);
    return ESP_OK;
}

/* ── media wrappers (prelude.h) ── */

static bool has_prefix(const char *p, const char *pre)
{
    size_t n = strlen(pre);
    return strncmp(p, pre, n) == 0 && (p[n] == '\0' || p[n] == '/');
}
static bool is_flash(const char *p) { return p != NULL && has_prefix(p, EVSTORE_MOUNT); }
static bool is_sd(const char *p)    { return p != NULL && has_prefix(p, SD_MOUNT_POINT); }

static __thread int t_sd_refs;
int h_sd_refs_held(void) { return t_sd_refs; }

static void note_sd_op(const char *path, const char *op)
{
    if (!is_sd(path)) return;
    h_mstats.sd_ops++;
    if (strcmp(h_task_name(), "test") != 0 && t_sd_refs == 0) {
        h_mstats.sd_ops_outside_bracket++;
        h_trace("sd_op_unbracketed", op, 0);
    }
}

FILE *h_fopen(const char *path, const char *mode)
{
    bool writing = strpbrk(mode, "wax+") != NULL;
    if (writing && is_flash(path)) {
        struct stat st;
        bool exists = stat(path, &st) == 0;
        if (!exists && h_dir_used_bytes(EVSTORE_MOUNT) + 4096U > h_media.flash_total) {
            h_mstats.flash_enospc++;
            errno = ENOSPC;
            return NULL;
        }
    }
    note_sd_op(path, "fopen");
    if (writing && is_sd(path) && strchr(mode, 'x') != NULL) {
        const char *b = strrchr(path, '/');
        h_trace("sd_create", b ? b + 1 : path, 0);
    }
    return fopen(path, mode);
}

static char g_flash_abs[PATH_MAX];
static bool fd_is_flash(int fd)
{
    if (g_flash_abs[0] == '\0') {
        if (realpath(EVSTORE_MOUNT, g_flash_abs) == NULL) return false;
    }
    char link[64], target[PATH_MAX];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    return has_prefix(target, g_flash_abs);
}

int h_fsync(int fd)
{
    if (fd_is_flash(fd)) {
        if (h_media.flash_eio_next_fsync) {
            h_media.flash_eio_next_fsync = false;
            h_mstats.flash_eio++;
            errno = EIO;
            return -1;
        }
        if (h_dir_used_bytes(EVSTORE_MOUNT) > h_media.flash_total) {
            h_mstats.flash_enospc++;
            errno = ENOSPC;
            return -1;
        }
        char link[64], target[PATH_MAX];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n > 0) {
            target[n] = '\0';
            const char *b = strrchr(target, '/');
            if (b != NULL && strcmp(b + 1, "quarantine.log") == 0) h_trace("fsync_quarantine", b + 1, 0);
        }
    }
    return 0;   /* no power-loss model in this harness: skip the host fsync */
}

int h_rename(const char *from, const char *to)  { note_sd_op(from, "rename"); return rename(from, to); }
int h_remove(const char *path)                  { note_sd_op(path, "remove"); return remove(path); }
int h_stat(const char *path, struct stat *st)   { note_sd_op(path, "stat"); return stat(path, st); }
DIR *h_opendir(const char *path)                { note_sd_op(path, "opendir"); return opendir(path); }
int h_mkdir(const char *path, mode_t mode)      { note_sd_op(path, "mkdir"); return mkdir(path, mode); }

/* ── sd_card.h ── */

esp_err_t sdcard_init_default(void) { return ESP_OK; }
esp_err_t sdcard_mount(void) { h_media.sd_mounted = true; return ESP_OK; }
esp_err_t sdcard_unmount(void)
{
    if (h_mstats.refs_now > 0) { h_mstats.unmount_with_refs++; return ESP_ERR_TIMEOUT; }
    h_media.sd_mounted = false;
    return ESP_OK;
}
bool sdcard_is_mounted(void) { return h_media.sd_mounted; }
esp_err_t sdcard_free_bytes(uint64_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!h_media.sd_mounted) return ESP_ERR_INVALID_STATE;
    uint64_t used = h_dir_used_bytes(SD_MOUNT_POINT);
    *out = h_media.sd_total > used ? h_media.sd_total - used : 0;
    return ESP_OK;
}
uint32_t sdcard_card_serial(void) { return h_media.sd_mounted ? h_media.sd_cid : 0; }
esp_err_t sdcard_start_monitor(uint32_t period_ms, sdcard_state_cb_t cb) { (void)period_ms; (void)cb; return ESP_OK; }
void sdcard_report_io_error(void) { h_mstats.report_err++; }
void sdcard_report_io_ok(void) { h_mstats.report_ok++; }
bool sdcard_io_lost(void) { return h_media.sd_io_lost; }
bool sdcard_io_begin(void)
{
    if (!h_media.sd_mounted || h_media.sd_io_lost) { h_mstats.io_begin_refused++; return false; }
    t_sd_refs++;
    h_mstats.refs_now++;
    if (h_mstats.refs_now > h_mstats.refs_max) h_mstats.refs_max = h_mstats.refs_now;
    h_mstats.io_begin++;
    return true;
}
void sdcard_io_end(void)
{
    if (t_sd_refs <= 0 || h_mstats.refs_now <= 0) fatal("sdcard_io_end without a matching sdcard_io_begin");
    t_sd_refs--;
    h_mstats.refs_now--;
    h_mstats.io_end++;
}
void sdcard_monitor_suspend(void) {}
void sdcard_monitor_resume(void) {}

/* ── NVS: map + commit-persisted image ── */

typedef struct {
    char ns[16], key[16];
    int type;            /* 1 u32 2 i32 3 u64 4 i64 5 str 6 blob */
    uint8_t *data;
    size_t len;
} nvs_ent_t;

static nvs_ent_t *g_nvs;
static size_t g_nnvs, g_capnvs;
static char g_nvs_path[PATH_MAX] = "nvs.dat";
static bool g_nvs_loaded;
static char g_handles[64][16];
static int g_nhandles;

void h_nvs_set_path(const char *path) { snprintf(g_nvs_path, sizeof g_nvs_path, "%s", path); g_nvs_loaded = false; }

static nvs_ent_t *nvs_find(const char *ns, const char *key)
{
    for (size_t i = 0; i < g_nnvs; i++)
        if (strcmp(g_nvs[i].ns, ns) == 0 && strcmp(g_nvs[i].key, key) == 0) return &g_nvs[i];
    return NULL;
}

static nvs_ent_t *nvs_put(const char *ns, const char *key, int type, const void *data, size_t len)
{
    nvs_ent_t *e = nvs_find(ns, key);
    if (e == NULL) {
        if (g_nnvs == g_capnvs) {
            g_capnvs = g_capnvs ? g_capnvs * 2 : 32;
            g_nvs = realloc(g_nvs, g_capnvs * sizeof *g_nvs);
            if (g_nvs == NULL) fatal("oom");
        }
        e = &g_nvs[g_nnvs++];
        memset(e, 0, sizeof *e);
        snprintf(e->ns, sizeof e->ns, "%s", ns);
        snprintf(e->key, sizeof e->key, "%s", key);
    }
    free(e->data);
    e->type = type;
    e->len = len;
    e->data = malloc(len ? len : 1);
    if (e->data == NULL) fatal("oom");
    memcpy(e->data, data, len);
    return e;
}

static void nvs_load(void)
{
    if (g_nvs_loaded) return;
    g_nvs_loaded = true;
    for (size_t i = 0; i < g_nnvs; i++) free(g_nvs[i].data);
    g_nnvs = 0;
    FILE *f = fopen(g_nvs_path, "rb");
    if (f == NULL) return;
    char ns[16], key[16];
    int type;
    size_t len;
    while (fscanf(f, "%15s %15s %d %zu ", ns, key, &type, &len) == 4) {
        uint8_t *buf = malloc(len ? len : 1);
        if (buf == NULL) fatal("oom");
        for (size_t i = 0; i < len; i++) {
            unsigned v;
            if (fscanf(f, "%2x", &v) != 1) fatal("corrupt nvs image");
            buf[i] = (uint8_t)v;
        }
        nvs_put(ns, key, type, buf, len);
        free(buf);
    }
    fclose(f);
}

static void nvs_save(void)
{
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_nvs_path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) fatal("nvs image write failed");
    for (size_t i = 0; i < g_nnvs; i++) {
        fprintf(f, "%s %s %d %zu ", g_nvs[i].ns, g_nvs[i].key, g_nvs[i].type, g_nvs[i].len);
        for (size_t j = 0; j < g_nvs[i].len; j++) fprintf(f, "%02x", g_nvs[i].data[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    if (rename(tmp, g_nvs_path) != 0) fatal("nvs image rename failed");
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    nvs_load();
    if (mode == NVS_READONLY) {
        bool any = false;
        for (size_t i = 0; i < g_nnvs; i++) if (strcmp(g_nvs[i].ns, ns) == 0) { any = true; break; }
        if (!any) return ESP_ERR_NVS_NOT_FOUND;
    }
    int h = -1;
    for (int i = 0; i < g_nhandles; i++) if (strcmp(g_handles[i], ns) == 0) { h = i; break; }
    if (h < 0) {
        if (g_nhandles >= 64) fatal("too many nvs namespaces");
        h = g_nhandles++;
        snprintf(g_handles[h], sizeof g_handles[h], "%s", ns);
    }
    *out = (nvs_handle_t)(h + 1);
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
static const char *hns(nvs_handle_t h)
{
    if (h == 0 || (int)h > g_nhandles) fatal("bad nvs handle");
    return g_handles[h - 1];
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    const char *ns = hns(h);
    nvs_save();
    if (strcmp(ns, "evlog") == 0) {
        nvs_ent_t *e = nvs_find("evlog", "cur");
        if (e != NULL && e->len >= 8) {
            uint32_t v[2];
            memcpy(v, e->data, sizeof v);
            h_trace("nvs_cursor", "", (int64_t)v[0] * 100000000LL + v[1]);
        }
    }
    return ESP_OK;
}

#define NVS_SET(fn, T, code) \
    esp_err_t fn(nvs_handle_t h, const char *k, T v) { nvs_put(hns(h), k, code, &v, sizeof v); return ESP_OK; }
NVS_SET(nvs_set_u32, uint32_t, 1)
NVS_SET(nvs_set_i32, int32_t, 2)
NVS_SET(nvs_set_u64, uint64_t, 3)
NVS_SET(nvs_set_i64, int64_t, 4)
#define NVS_GET(fn, T, code) \
    esp_err_t fn(nvs_handle_t h, const char *k, T *v) { \
        nvs_ent_t *e = nvs_find(hns(h), k); \
        if (e == NULL) return ESP_ERR_NVS_NOT_FOUND; \
        if (e->type != code) return ESP_ERR_NVS_TYPE_MISMATCH; \
        memcpy(v, e->data, sizeof *v); return ESP_OK; }
NVS_GET(nvs_get_u32, uint32_t, 1)
NVS_GET(nvs_get_i32, int32_t, 2)
NVS_GET(nvs_get_u64, uint64_t, 3)
NVS_GET(nvs_get_i64, int64_t, 4)

esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { nvs_put(hns(h), k, 5, v, strlen(v) + 1); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { nvs_put(hns(h), k, 6, v, len); return ESP_OK; }
static esp_err_t nvs_get_var(nvs_handle_t h, const char *k, int type, void *out, size_t *len)
{
    nvs_ent_t *e = nvs_find(hns(h), k);
    if (e == NULL) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (out == NULL) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len) { return nvs_get_var(h, k, 5, out, len); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len) { return nvs_get_var(h, k, 6, out, len); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    const char *ns = hns(h);
    for (size_t i = 0; i < g_nnvs; i++) {
        if (strcmp(g_nvs[i].ns, ns) == 0 && strcmp(g_nvs[i].key, k) == 0) {
            free(g_nvs[i].data);
            g_nvs[i] = g_nvs[--g_nnvs];
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

/* ── heap ── */
size_t heap_caps_get_free_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM) return 6U * 1024 * 1024;
    if (caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return 180U * 1024;
    return 6U * 1024 * 1024;
}
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    if (caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return h_media.heap_internal_largest;
    return 4U * 1024 * 1024;
}
size_t heap_caps_get_total_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM) return 8U * 1024 * 1024;   /* PSRAM enumerated: normal caps */
    return 320U * 1024;
}
uint32_t esp_get_free_heap_size(void) { return 200000; }

/* ── system / identity ── */
void esp_restart(void)
{
    fprintf(stderr, "[stubs] FATAL: production code called esp_restart() (self-reboot) at vt=%llu us\n",
            (unsigned long long)h_vt_us());
    abort();
}

static esp_app_desc_t g_app;
const esp_app_desc_t *esp_app_get_description(void)
{
    snprintf(g_app.version, sizeof g_app.version, "%s", h_media.app_version);
    snprintf(g_app.project_name, sizeof g_app.project_name, "ambyte-iot");
    return &g_app;
}

esp_err_t esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6])
{
    (void)ifx;
    static const uint8_t m[6] = { 0x24, 0x0A, 0xC4, 0xE0, 0x51, 0x7A };
    memcpy(mac, m, 6);
    return ESP_OK;
}
bool wifi_manager_is_connected(void) { return true; }
esp_err_t wifi_manager_is_provisioned(bool *out) { if (out) *out = true; return ESP_OK; }

esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t ch, uint32_t idle) { (void)mode; (void)ch; (void)idle; return ESP_OK; }

struct esp_pm_lock { int n; };
static struct esp_pm_lock g_pm_lock;
esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type, int arg, const char *name, esp_pm_lock_handle_t *out)
{
    (void)type; (void)arg; (void)name;
    *out = &g_pm_lock;
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h) { h->n++; return ESP_OK; }
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h) { h->n--; return ESP_OK; }

uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len)
{
    (void)crc; (void)buf; (void)len;
    fatal("esp_rom_crc32_le reached (AMBIT calibration path is outside this harness)");
    return 0;
}
tdefl_status tdefl_init(tdefl_compressor *d, tdefl_put_buf_func_ptr f, void *u, int flags)
{
    (void)d; (void)f; (void)u; (void)flags;
    fatal("tdefl_init reached (publish_gzip is never enabled in this harness)");
    return TDEFL_STATUS_BAD_PARAM;
}
tdefl_status tdefl_compress(tdefl_compressor *d, const void *in, size_t *in_sz, void *out, size_t *out_sz, tdefl_flush fl)
{
    (void)d; (void)in; (void)in_sz; (void)out; (void)out_sz; (void)fl;
    fatal("tdefl_compress reached");
    return TDEFL_STATUS_BAD_PARAM;
}

/* ── clock_trust / fleet_jitter / ota / timezone ── */
esp_err_t clock_trust_boot_guard(bool s, bool *a, time_t *f) { (void)s; if (a) *a = false; if (f) *f = 0; return ESP_OK; }
esp_err_t clock_trust_refresh_hwm(void) { return ESP_OK; }
esp_err_t clock_trust_refresh_hwm_at(time_t t) { (void)t; return ESP_OK; }
esp_err_t clock_trust_set_hwm_authoritative(time_t t) { (void)t; return ESP_OK; }
void clock_trust_note_sntp(void) {}
void clock_trust_note_rtc(void) {}
static const char *g_clock_source = "rtc";
void h_set_clock_source(const char *s) { g_clock_source = s; }
void clock_trust_get_status(const char **src, bool *suspect)
{
    if (src) *src = g_clock_source;
    if (suspect) *suspect = false;
}
esp_err_t fleet_jitter_slot_for_mac(const uint8_t mac[6], uint32_t n, uint32_t *slot) { (void)mac; (void)n; *slot = 0; return ESP_OK; }
esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t n, uint32_t *slot) { (void)n; *slot = 0; return ESP_OK; }
bool ota_update_in_progress(void) { return false; }
int32_t timezone_utc_offset_seconds(int64_t utc) { (void)utc; return 0; }
int64_t timezone_localize(int64_t utc) { return utc; }

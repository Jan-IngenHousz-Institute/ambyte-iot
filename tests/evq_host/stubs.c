/* Host stubs for the storage harness: FreeRTOS (single-threaded, test clock),
 * NVS (committed-only persistence), sd_card, esp_littlefs_info, heap caps,
 * logging, and the fault hook. Plain libc (no forced shim include). */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sd_card.h"

#include "evq_host.h"

/* ── logging ─────────────────────────────────────────────────────────────── */
void evq_host_log(char level, const char *tag, const char *fmt, ...)
{
    static int quiet = -1;
    if (quiet < 0) quiet = getenv("EVQ_QUIET") != NULL && getenv("EVQ_VERBOSE") == NULL;
    if (quiet && level == 'I') return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c (%s) ", level, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

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
    case ESP_ERR_NOT_FINISHED: return "ESP_ERR_NOT_FINISHED";
    default: return "ESP_ERR_?";
    }
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        const char *e = getenv("EVQ_NO_PSRAM");
        return (e && *e == '1') ? 0 : 2u * 1024 * 1024;
    }
    return 512u * 1024;
}

esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    (void)label;
    return shim_flash_info(total, used) == 0 ? ESP_OK : ESP_FAIL;
}

/* ── FreeRTOS ────────────────────────────────────────────────────────────── */
static uint32_t s_clock = 1000;
void     evq_clock_advance(uint32_t ms) { s_clock += ms; }
uint32_t evq_clock_now(void) { return s_clock; }
TickType_t xTaskGetTickCount(void) { return s_clock; }
void vTaskDelay(TickType_t t) { s_clock += t; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = NULL;
    return pdFAIL;                 /* the storage harness drives the keeper directly */
}
void vTaskDelete(TaskHandle_t t) { (void)t; }
BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)t; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear; (void)wait; return 0; }

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf)
{
    buf->held = 0;
    return buf;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    (void)wait;
    if (s->held) {
        fprintf(stderr, "FATAL: s_mtx re-taken by its holder (self-deadlock on target)\n");
        abort();
    }
    if (evq_sd_stub_refs() > 0) {
        fprintf(stderr, "FATAL: lock-order violation: s_mtx taken while an SD ref is held\n");
        abort();
    }
    s->held = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (!s->held) { fprintf(stderr, "FATAL: give of an unheld mutex\n"); abort(); }
    s->held = 0;
    return pdTRUE;
}

/* ── NVS: committed-only persistence in ./nvs.txt ────────────────────────── */
typedef struct { char ns[16]; char key[16]; size_t len; uint8_t val[64]; bool used; } nvs_ent_t;
#define NVS_MAX 64
static nvs_ent_t s_nvs[NVS_MAX];        /* committed image */
static nvs_ent_t s_nvs_pend[NVS_MAX];   /* working copy (sets land here) */
static bool s_nvs_loaded = false;
static char s_nvs_ns[8][16];

static void nvs_load(void)
{
    if (s_nvs_loaded) return;
    s_nvs_loaded = true;
    FILE *f = fopen("nvs.txt", "r");
    if (f != NULL) {
        char ns[16], key[16], hex[160];
        int i = 0;
        while (i < NVS_MAX && fscanf(f, "%15s %15s %159s", ns, key, hex) == 3) {
            nvs_ent_t *e = &s_nvs[i++];
            e->used = true;
            snprintf(e->ns, sizeof e->ns, "%s", ns);
            snprintf(e->key, sizeof e->key, "%s", key);
            e->len = strlen(hex) / 2;
            for (size_t k = 0; k < e->len && k < sizeof e->val; k++) {
                unsigned b; sscanf(hex + 2 * k, "%2x", &b); e->val[k] = (uint8_t)b;
            }
        }
        fclose(f);
    }
    memcpy(s_nvs_pend, s_nvs, sizeof s_nvs);
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    nvs_load();
    for (int i = 0; i < 8; i++) {
        if (s_nvs_ns[i][0] == '\0') snprintf(s_nvs_ns[i], sizeof s_nvs_ns[i], "%s", ns);
        if (strcmp(s_nvs_ns[i], ns) == 0) { *out = (nvs_handle_t)(i + 1); return ESP_OK; }
    }
    return ESP_FAIL;
}
void nvs_close(nvs_handle_t h) { (void)h; }

static nvs_ent_t *nvs_find(nvs_ent_t *tab, nvs_handle_t h, const char *key, bool create)
{
    const char *ns = s_nvs_ns[h - 1];
    for (int i = 0; i < NVS_MAX; i++) {
        if (tab[i].used && strcmp(tab[i].ns, ns) == 0 && strcmp(tab[i].key, key) == 0) return &tab[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < NVS_MAX; i++) {
        if (!tab[i].used) {
            tab[i].used = true;
            snprintf(tab[i].ns, sizeof tab[i].ns, "%s", ns);
            snprintf(tab[i].key, sizeof tab[i].key, "%s", key);
            return &tab[i];
        }
    }
    return NULL;
}

static esp_err_t nvs_get(nvs_handle_t h, const char *key, void *out, size_t want)
{
    /* Reads see pending sets (as ESP-IDF NVS does: set_* writes immediately). */
    nvs_ent_t *e = nvs_find(s_nvs_pend, h, key, false);
    if (e == NULL) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != want) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->val, want);
    return ESP_OK;
}
static esp_err_t nvs_set(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    nvs_ent_t *e = nvs_find(s_nvs_pend, h, key, true);
    if (e == NULL || len > sizeof e->val) return ESP_FAIL;
    memcpy(e->val, v, len);
    e->len = len;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *o) { return nvs_get(h, k, o, 4); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return nvs_set(h, k, &v, 4); }
esp_err_t nvs_get_u64(nvs_handle_t h, const char *k, uint64_t *o) { return nvs_get(h, k, o, 8); }
esp_err_t nvs_set_u64(nvs_handle_t h, const char *k, uint64_t v) { return nvs_set(h, k, &v, 8); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *len)
{
    nvs_ent_t *e = nvs_find(s_nvs_pend, h, k, false);
    if (e == NULL) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(o, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return nvs_set(h, k, v, len); }

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    memcpy(s_nvs, s_nvs_pend, sizeof s_nvs);
    FILE *f = fopen("nvs.txt.tmp", "w");
    if (f == NULL) return ESP_FAIL;
    for (int i = 0; i < NVS_MAX; i++) {
        if (!s_nvs[i].used) continue;
        fprintf(f, "%s %s ", s_nvs[i].ns, s_nvs[i].key);
        for (size_t k = 0; k < s_nvs[i].len; k++) fprintf(f, "%02x", s_nvs[i].val[k]);
        fputc('\n', f);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename("nvs.txt.tmp", "nvs.txt") == 0 ? ESP_OK : ESP_FAIL;
}

/* ── sd_card ─────────────────────────────────────────────────────────────── */
static bool     s_sd_loaded = false;
static bool     s_sd_mounted = false;
static uint32_t s_sd_cid = 0;
static bool     s_sd_lost = false;
static int      s_sd_refs = 0;

static void sd_load(void)
{
    if (s_sd_loaded) return;
    s_sd_loaded = true;
    FILE *f = fopen(".shim/sd_state", "r");
    unsigned m = 1, cid = 0xC1D00001u;
    if (f != NULL) { if (fscanf(f, "%u %x", &m, &cid) != 2) { m = 1; cid = 0xC1D00001u; } fclose(f); }
    struct stat st;
    s_sd_mounted = m != 0 && stat("sdcard", &st) == 0;
    s_sd_cid = cid;
}

static void sd_save(void)
{
    mkdir(".shim", 0777);
    FILE *f = fopen(".shim/sd_state", "w");
    if (f != NULL) { fprintf(f, "%u %08x\n", s_sd_mounted ? 1u : 0u, (unsigned)s_sd_cid); fclose(f); }
}

bool     evq_sd_stub_mounted(void) { sd_load(); return s_sd_mounted; }
uint32_t evq_sd_stub_cid(void) { sd_load(); return s_sd_cid; }
int      evq_sd_stub_refs(void) { return s_sd_refs; }
void     evq_sd_stub_set(bool mounted, uint32_t cid) { sd_load(); s_sd_mounted = mounted; s_sd_cid = cid; sd_save(); }
void     evq_sd_stub_set_lost(bool lost) { s_sd_lost = lost; }

bool sdcard_is_mounted(void) { sd_load(); return s_sd_mounted; }
uint32_t sdcard_card_serial(void) { sd_load(); return s_sd_mounted ? s_sd_cid : 0; }
esp_err_t sdcard_free_bytes(uint64_t *out)
{
    if (!sdcard_is_mounted()) return ESP_ERR_INVALID_STATE;
    *out = (uint64_t)shim_sd_free();
    return ESP_OK;
}
bool sdcard_io_begin(void)
{
    sd_load();
    if (!s_sd_mounted || s_sd_lost) return false;
    s_sd_refs++;
    return true;
}
void sdcard_io_end(void)
{
    if (s_sd_refs <= 0) { fprintf(stderr, "FATAL: sdcard_io_end without begin\n"); abort(); }
    s_sd_refs--;
}
void sdcard_report_io_error(void) {}
void sdcard_report_io_ok(void) {}
bool sdcard_io_lost(void) { return s_sd_lost; }

/* ── fault hook ──────────────────────────────────────────────────────────── */
static char     s_fault_name[96];
static unsigned s_fault_nth = 0;
static char     s_fault_mode[16];
static unsigned s_fault_hits = 0;
static bool     s_fault_parsed = false;

static void fault_parse(void)
{
    if (s_fault_parsed) return;
    s_fault_parsed = true;
    const char *e = getenv("EVQ_FAULT");
    if (e == NULL || *e == '\0') return;
    char buf[160];
    snprintf(buf, sizeof buf, "%s", e);
    char *a = strtok(buf, ":"), *b = strtok(NULL, ":"), *c = strtok(NULL, ":");
    if (a && b && c) {
        snprintf(s_fault_name, sizeof s_fault_name, "%s", a);
        s_fault_nth = (unsigned)atoi(b);
        snprintf(s_fault_mode, sizeof s_fault_mode, "%s", c);
    }
}

/* Hit counts per point, written at normal exit (dry runs size nth=last). */
static char     s_cnt_name[128][96];
static unsigned s_cnt_val[128];
static int      s_cnt_n = 0;
static bool     s_cnt_hooked = false;
static void fault_counts_dump(void)
{
    FILE *f = fopen(".shim/fault_counts", "a");
    if (f == NULL) return;
    for (int i = 0; i < s_cnt_n; i++) fprintf(f, "%s %u\n", s_cnt_name[i], s_cnt_val[i]);
    fclose(f);
}

void evq_fault_point(const char *name)
{
    fault_parse();
    if (!s_cnt_hooked) { s_cnt_hooked = true; atexit(fault_counts_dump); }
    {
        int i;
        for (i = 0; i < s_cnt_n; i++) if (strcmp(s_cnt_name[i], name) == 0) break;
        if (i == s_cnt_n && s_cnt_n < 128) { snprintf(s_cnt_name[s_cnt_n], sizeof s_cnt_name[0], "%s", name); s_cnt_val[s_cnt_n++] = 0; }
        if (i < s_cnt_n) s_cnt_val[i]++;
    }
    static char seen[64][96];
    static int nseen = 0;
    bool first = true;
    for (int i = 0; i < nseen; i++) if (strcmp(seen[i], name) == 0) { first = false; break; }
    if (first && nseen < 64) {
        snprintf(seen[nseen++], sizeof seen[0], "%s", name);
        mkdir(".shim", 0777);
        FILE *f = fopen(".shim/faults_reached", "a");
        if (f) { fprintf(f, "%s\n", name); fclose(f); }
    }
    if (s_fault_name[0] == '\0' || strcmp(name, s_fault_name) != 0) return;
    /* hits of the ARMED point accumulate across boots (the harness keeps it
     * armed until it fires), so nth counts the same way as the dry run */
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        FILE *hf = fopen(".shim/fault_hits", "r");
        if (hf) { if (fscanf(hf, "%u", &s_fault_hits) != 1) s_fault_hits = 0; fclose(hf); }
    }
    ++s_fault_hits;
    FILE *hf = fopen(".shim/fault_hits", "w");
    if (hf) { fprintf(hf, "%u\n", s_fault_hits); fclose(hf); }
    if (s_fault_hits != s_fault_nth) return;
    FILE *f = fopen(".shim/fault_fired", "a");
    if (f) { fprintf(f, "%s:%u:%s\n", name, s_fault_nth, s_fault_mode); fclose(f); }
    if (strcmp(s_fault_mode, "crash") == 0) {
        if (strstr(name, "inside") != NULL) { shim_arm_crash_inside(); return; }
        shim_mark("fault-crash");
        evq_host_flush_manifests();
        _exit(86);
    } else if (strcmp(s_fault_mode, "remove") == 0) {
        /* card pulled at this exact point (no crash) */
        evq_sd_stub_set(false, evq_sd_stub_cid());
    } else if (strcmp(s_fault_mode, "eio") == 0) {
        shim_arm_errno(EIO);
    } else if (strcmp(s_fault_mode, "enospc") == 0) {
        shim_arm_errno(ENOSPC);
    }
}

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
#include "sd_diag.h"

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

/* The event_log cursor as committed (or as found at boot), for the C-18/C-19
 * trace oracle: blob `evlog/cur` {seq, off, crc} and legacy rd_seq/rd_off. */
static void nvs_note_cursor(const char *kind)
{
    long long bs = -1, bo = -1, ls = -1, lo = -1;
    for (int i = 0; i < NVS_MAX; i++) {
        const nvs_ent_t *e = &s_nvs[i];
        if (!e->used || strcmp(e->ns, "evlog") != 0) continue;
        uint32_t a = 0, b = 0;
        if (strcmp(e->key, "cur") == 0 && e->len == 12) {
            memcpy(&a, e->val, 4); memcpy(&b, e->val + 4, 4);
            bs = a; bo = b;
        } else if (strcmp(e->key, "rd_seq") == 0 && e->len == 4) {
            memcpy(&a, e->val, 4); ls = a;
        } else if (strcmp(e->key, "rd_off") == 0 && e->len == 4) {
            memcpy(&a, e->val, 4); lo = a;
        }
    }
    char buf[200];
    snprintf(buf, sizeof buf, "{\"op\":\"nvs\",\"kind\":\"%s\",\"blob\":[%lld,%lld],\"legacy\":[%lld,%lld]}",
             kind, bs, bo, ls, lo < 0 && ls >= 0 ? 0 : lo);
    shim_note(buf);
}

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
    nvs_note_cursor("load");
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
    esp_err_t rc = rename("nvs.txt.tmp", "nvs.txt") == 0 ? ESP_OK : ESP_FAIL;
    if (rc == ESP_OK) nvs_note_cursor("commit");
    return rc;
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
/* EVQ_FAULT = "<point>:<nth>:<mode>[,<point>:<nth>:<mode>...]" (≤ 4 specs).
 * One spec behaves exactly as before; several let one run arm faults at
 * different points (e.g. the commit rename AND the pair-retirement rename that
 * follows it). Hits per spec accumulate across boots in .shim/fault_hits, one
 * line per spec in spec order (a single spec keeps the old one-number file). */
#define FAULT_SPECS 4
static char     s_fault_name[FAULT_SPECS][96];
static unsigned s_fault_nth[FAULT_SPECS];
static char     s_fault_mode[FAULT_SPECS][16];
static unsigned s_fault_hits[FAULT_SPECS];
static int      s_fault_n = 0;
static bool     s_fault_parsed = false;

static void fault_parse(void)
{
    if (s_fault_parsed) return;
    s_fault_parsed = true;
    const char *e = getenv("EVQ_FAULT");
    if (e == NULL || *e == '\0') return;
    char buf[400];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL;
    for (char *spec = strtok_r(buf, ",", &save); spec != NULL && s_fault_n < FAULT_SPECS;
         spec = strtok_r(NULL, ",", &save)) {
        char *s2 = NULL;
        char *a = strtok_r(spec, ":", &s2), *b = strtok_r(NULL, ":", &s2), *c = strtok_r(NULL, ":", &s2);
        if (a && b && c) {
            snprintf(s_fault_name[s_fault_n], sizeof s_fault_name[0], "%s", a);
            s_fault_nth[s_fault_n] = (unsigned)atoi(b);
            snprintf(s_fault_mode[s_fault_n], sizeof s_fault_mode[0], "%s", c);
            s_fault_n++;
        }
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
    /* Keeper-side points go into the ops trace (C-19 context: e.g. an SD
     * remove right after import.after_fsync_before_sd_remove is a legacy
     * import handing its records to flash). Store/ACK points are hot and
     * carry no such meaning: not logged. */
    static const char *const ctx[] = { "import.", "reimport.", "reclaim.", "archive.", "boot.", "quarantine.", "compact.",
                                       "retire." };
    for (size_t i = 0; i < sizeof ctx / sizeof ctx[0]; i++) {
        if (strncmp(name, ctx[i], strlen(ctx[i])) == 0) {
            char buf[160];
            snprintf(buf, sizeof buf, "{\"op\":\"fp\",\"name\":\"%s\"}", name);
            shim_note(buf);
            break;
        }
    }
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
    int fi = -1;
    for (int i = 0; i < s_fault_n; i++) if (strcmp(name, s_fault_name[i]) == 0) { fi = i; break; }
    if (fi < 0) return;
    /* hits of an ARMED point accumulate across boots (the harness keeps it
     * armed until it fires), so nth counts the same way as the dry run */
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        FILE *hf = fopen(".shim/fault_hits", "r");
        if (hf) {
            for (int i = 0; i < s_fault_n; i++) if (fscanf(hf, "%u", &s_fault_hits[i]) != 1) s_fault_hits[i] = 0;
            fclose(hf);
        }
    }
    ++s_fault_hits[fi];
    FILE *hf = fopen(".shim/fault_hits", "w");
    if (hf) { for (int i = 0; i < s_fault_n; i++) fprintf(hf, "%u\n", s_fault_hits[i]); fclose(hf); }
    if (s_fault_hits[fi] != s_fault_nth[fi]) return;
    const char *mode = s_fault_mode[fi];
    FILE *f = fopen(".shim/fault_fired", "a");
    if (f) { fprintf(f, "%s:%u:%s\n", name, s_fault_nth[fi], mode); fclose(f); }
    {
        char buf[200];
        snprintf(buf, sizeof buf, "{\"op\":\"fault_fired\",\"name\":\"%s\",\"mode\":\"%s\"}", name, mode);
        shim_note(buf);
    }
    if (strcmp(mode, "crash") == 0) {
        if (strstr(name, "inside") != NULL) { shim_arm_crash_inside(); return; }
        shim_mark("fault-crash");
        evq_host_flush_manifests();
        _exit(86);
    } else if (strcmp(mode, "crash_both") == 0) {
        /* crash INSIDE the next rename with the FAT "both names" outcome forced */
        shim_arm_crash_inside_outcome(2);
    } else if (strcmp(mode, "remove") == 0) {
        /* card pulled at this exact point (no crash) */
        evq_sd_stub_set(false, evq_sd_stub_cid());
    } else if (strcmp(mode, "eio") == 0) {
        shim_arm_errno(EIO);
    } else if (strcmp(mode, "eio2") == 0) {
        shim_arm_errno_n(EIO, 2);      /* the next TWO media ops fail (e.g. a rename and its restore) */
    } else if (strcmp(mode, "enospc") == 0) {
        shim_arm_errno(ENOSPC);
    } else if (shim_arm_special(mode) != 0) {
        fprintf(stderr, "FATAL: unknown fault mode %s\n", mode);
        abort();
    }
}

/* ── sd_diag (device API over the pure core, sd_diag_core.c) ─────────────────
 * event_log.c calls sd_diag_fault/sd_diag_refusal (HEAD only; baselines never
 * do). The block is kept in .shim/sd_diag.bin, written through on every change:
 * that models the RTC_NOINIT block surviving the harness's reboots (a CPU
 * reset keeps counting, diag_exact=1). The simulated power-loss transform is a
 * media durability model only; it does not model an RTC wipe. Never SD I/O. */
static sd_diag_block_t s_diag;
static bool s_diag_loaded = false;

static void diag_load(void)
{
    if (s_diag_loaded) return;
    s_diag_loaded = true;
    FILE *f = fopen(".shim/sd_diag.bin", "rb");
    bool have = false;
    if (f != NULL) { have = fread(&s_diag, 1, sizeof s_diag, f) == sizeof s_diag; fclose(f); }
    if (have && sd_diag_valid(&s_diag)) {
        sd_diag_core_boot(&s_diag, true);
    } else {
        memset(&s_diag, 0, sizeof s_diag);
        sd_diag_core_boot(&s_diag, false);
        sd_diag_core_merge_floor(&s_diag, NULL);            /* no floor: epoch 1, inexact */
    }
}

static void diag_save(void)
{
    mkdir(".shim", 0777);
    FILE *f = fopen(".shim/sd_diag.bin.tmp", "wb");
    if (f == NULL) return;
    (void)fwrite(&s_diag, 1, sizeof s_diag, f);
    fclose(f);
    rename(".shim/sd_diag.bin.tmp", ".shim/sd_diag.bin");
}

void sd_diag_fault(sd_diag_writer_t w, sd_diag_op_t op, int err)
{
    diag_load();
    uint64_t sd0 = shim_sd_ops();
    sd_diag_core_fault(&s_diag, w, op, err, evq_clock_now());
    diag_save();
    if (shim_sd_ops() != sd0) { fprintf(stderr, "FATAL: sd_diag performed SD I/O\n"); abort(); }   /* C-31 */
    char buf[160];
    snprintf(buf, sizeof buf, "{\"op\":\"diag_fault\",\"w\":\"%s\",\"dop\":\"%s\",\"errno\":%d}",
             sd_diag_writer_name((unsigned)w), sd_diag_op_name((unsigned)op), err);
    shim_note(buf);
}

void sd_diag_refusal(sd_diag_refusal_t reason, int64_t id, int err, uint8_t blocked, uint8_t sd_state)
{
    diag_load();
    uint64_t sd0 = shim_sd_ops();
    sd_diag_core_refusal(&s_diag, reason, id, err, blocked, sd_state, 1758000000000LL + (int64_t)evq_clock_now(),
                         evq_clock_now());
    diag_save();
    if (shim_sd_ops() != sd0) { fprintf(stderr, "FATAL: sd_diag performed SD I/O\n"); abort(); }   /* C-31 */
}

int evq_sd_diag_render(char *buf, size_t cap)
{
    diag_load();
    return sd_diag_render_json(&s_diag, buf, cap);
}

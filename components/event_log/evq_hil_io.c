/* HIL-only I/O wrappers + named fault points for event_log.c
 * (CONFIG_AMBYTE_EVQ_HIL). Compiled only with the flag (CMakeLists). The
 * release build never links this file; its EVQ_FAULT_POINT stays empty. */
#include "evq_hil_io_impl.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "evq_hil_trace.h"

/* The wrappers must call the REAL libc functions: this file does not include
 * evq_hil_io.h, so the names below are the plain ones. */

typedef struct {
    FILE    *f;
    uint8_t  kind;        /* 0 other, 1 sd record, 2 sd other, 3 index, 4 flash segment */
    bool     writable;
    bool     err;
    uint32_t bytes;
    char     path[64];
} hil_of_t;

#define HIL_MAX_OPEN 24
static hil_of_t s_of[HIL_MAX_OPEN];
static evq_tr_entry_t *s_ring_mem;

static int64_t now_us(void) { return esp_timer_get_time(); }

static uint8_t classify(const char *p)
{
    if (evq_tr_is_sd_record(p)) return 1;
    if (evq_tr_is_sd(p)) return 2;
    if (evq_tr_is_index(p)) return 3;
    if (evq_tr_is_flash_segment(p)) return 4;
    return 0;
}

static bool relevant(uint8_t k) { return k == 1 || k == 3 || k == 4; }

static hil_of_t *of_find(FILE *f)
{
    for (int i = 0; i < HIL_MAX_OPEN; i++) if (s_of[i].f == f && f != NULL) return &s_of[i];
    return NULL;
}

static hil_of_t *of_find_fd(int fd)
{
    for (int i = 0; i < HIL_MAX_OPEN; i++) if (s_of[i].f != NULL && fileno(s_of[i].f) == fd) return &s_of[i];
    return NULL;
}

void evq_hil_io_init(void)
{
    if (s_ring_mem == NULL) {
        s_ring_mem = heap_caps_calloc(EVQ_TR_CAP, sizeof(evq_tr_entry_t), MALLOC_CAP_SPIRAM);
        if (s_ring_mem == NULL) s_ring_mem = heap_caps_calloc(EVQ_TR_CAP / 8, sizeof(evq_tr_entry_t), MALLOC_CAP_8BIT);
        evq_tr_init(s_ring_mem);
    }
}

void evq_hil_rom_reset(const char *why)
{
    printf("\r\nHIL_FAULT fired %s us=%lld\r\n", why ? why : "-", (long long)now_us());
    fflush(stdout);
    esp_rom_delay_us(60000);           /* let the USB-serial FIFO drain */
    esp_rom_software_reset_system();   /* no shutdown handlers, no coredump */
    for (;;) { }
}

void evq_hil_fault_point(const char *name)
{
    evq_arm_mode_t act = evq_arm_hit(name);
    if (act == EVQ_ARM_RESET) {
        evq_tr_record(now_us(), EVQ_TR_FAULT, 0, 0, name, "reset");
        evq_hil_rom_reset(name);
    }
}

/* -1 = must reset midway (writes only), >0 = inject that errno, 0 = run. */
static int take_io(void) { return evq_arm_take_io(); }

FILE *evq_hil_fopen(const char *path, const char *mode)
{
    uint8_t k = classify(path);
    bool w = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    evq_io_counters_t *c = evq_io_counters();
    if (k == 1 || k == 2) { if (w) c->sd_write_open++; else c->sd_read_open++; }
    int inj = take_io();
    if (inj > 0) {
        errno = inj;
        if (relevant(k)) evq_tr_record(now_us(), w ? EVQ_TR_OPEN_W : EVQ_TR_OPEN_R, -inj, 0, path, "injected");
        return NULL;
    }
    FILE *f = fopen(path, mode);
    int e = f ? 0 : errno;
    if (relevant(k)) evq_tr_record(now_us(), w ? EVQ_TR_OPEN_W : EVQ_TR_OPEN_R, -e, 0, path, NULL);
    if (f != NULL) {
        for (int i = 0; i < HIL_MAX_OPEN; i++) {
            if (s_of[i].f == NULL) {
                s_of[i].f = f;
                s_of[i].kind = k;
                s_of[i].writable = w;
                s_of[i].err = false;
                s_of[i].bytes = 0;
                snprintf(s_of[i].path, sizeof s_of[i].path, "%s", path);
                break;
            }
        }
    }
    if (f == NULL) errno = e;
    return f;
}

int evq_hil_fclose(FILE *f)
{
    hil_of_t *o = of_find(f);
    int rc = fclose(f);
    int e = rc == 0 ? 0 : errno;
    if (o != NULL) {
        if (relevant(o->kind)) {
            evq_tr_record(now_us(), o->writable ? EVQ_TR_CLOSE_W : EVQ_TR_CLOSE_R, -e, o->bytes, o->path, NULL);
        }
        if (o->writable && (o->kind == 1 || o->kind == 2)) evq_io_counters()->sd_mut++;
        o->f = NULL;
    }
    if (rc != 0) errno = e;
    return rc;
}

size_t evq_hil_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    hil_of_t *o = of_find(f);
    int inj = take_io();
    if (inj == -1) {
        size_t half = (sz * n) / 2;
        (void)fwrite(p, 1, half, f);
        fflush(f);
        fsync(fileno(f));
        evq_hil_rom_reset("fwrite_inside");
    }
    if (inj > 0) {
        if (o) o->err = true;
        errno = inj;
        return 0;
    }
    size_t w = fwrite(p, sz, n, f);
    if (o != NULL) {
        o->bytes += (uint32_t)(w * sz);
        if (o->kind == 1 || o->kind == 2) evq_io_counters()->sd_mut++;
        if (o->kind == 4) evq_io_counters()->store_appends++;
        if (o->kind == 3 && w > 0) {
            char pre[EVQ_TR_NAME_MAX];
            size_t m = w * sz < sizeof pre - 1 ? w * sz : sizeof pre - 1;
            memcpy(pre, p, m);
            pre[m] = '\0';
            for (size_t i = 0; i < m; i++) if (pre[i] == '\n' || pre[i] == '\t') pre[i] = ' ';
            evq_tr_record(now_us(), EVQ_TR_IDX_WRITE, 0, (uint32_t)(w * sz), o->path, pre);
        }
    }
    return w;
}

size_t evq_hil_fread(void *p, size_t sz, size_t n, FILE *f)
{
    hil_of_t *o = of_find(f);
    int inj = take_io();
    if (inj > 0) {
        if (o) o->err = true;
        errno = inj;
        return 0;
    }
    size_t r = fread(p, sz, n, f);
    if (o != NULL) o->bytes += (uint32_t)(r * sz);
    return r;
}

int evq_hil_fflush(FILE *f)
{
    return fflush(f);
}

int evq_hil_fsync(int fd)
{
    hil_of_t *o = of_find_fd(fd);
    int inj = take_io();
    if (inj > 0) {
        if (o) o->err = true;
        errno = inj;
        if (o && relevant(o->kind)) evq_tr_record(now_us(), EVQ_TR_FSYNC, -inj, o->bytes, o->path, "injected");
        return -1;
    }
    int rc = fsync(fd);
    int e = rc == 0 ? 0 : errno;
    if (o != NULL) {
        if (o->kind == 4) evq_io_counters()->store_fsyncs++;
        if (relevant(o->kind) && o->kind != 4) evq_tr_record(now_us(), EVQ_TR_FSYNC, -e, o->bytes, o->path, NULL);
        if (o->kind == 1 || o->kind == 2) evq_io_counters()->sd_mut++;
    }
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_rename(const char *a, const char *b)
{
    uint8_t k = classify(a);
    int inj = take_io();
    if (inj == -1) evq_hil_rom_reset("rename_inside");
    if (inj > 0) { errno = inj; if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_RENAME, -inj, 0, a, b); return -1; }
    int rc = rename(a, b);
    int e = rc == 0 ? 0 : errno;
    if (relevant(k) || relevant(classify(b))) evq_tr_record(now_us(), EVQ_TR_RENAME, -e, 0, a, b);
    if (k == 1 || k == 2) evq_io_counters()->sd_mut++;
    else if (k == 3 || k == 4) evq_io_counters()->flash_mut++;
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_remove(const char *p)
{
    uint8_t k = classify(p);
    int inj = take_io();
    if (inj == -1) evq_hil_rom_reset("remove_inside");
    if (inj > 0) { errno = inj; if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_REMOVE, -inj, 0, p, "injected"); return -1; }
    int rc = remove(p);
    int e = rc == 0 ? 0 : errno;
    if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_REMOVE, -e, 0, p, NULL);
    if (k == 1 || k == 2) evq_io_counters()->sd_mut++;
    else if (k != 0) evq_io_counters()->flash_mut++;
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_mkdir(const char *p, mode_t m)
{
    uint8_t k = classify(p);
    int inj = take_io();
    if (inj > 0) { errno = inj; return -1; }
    int rc = mkdir(p, m);
    int e = rc == 0 ? 0 : errno;
    if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_MKDIR, -e, 0, p, NULL);
    if ((k == 1 || k == 2) && rc == 0) evq_io_counters()->sd_mut++;
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_stat(const char *p, struct stat *st)
{
    uint8_t k = classify(p);
    if (k == 1 || k == 2) evq_io_counters()->sd_read_open++;   /* any SD access counts */
    return stat(p, st);
}

DIR *evq_hil_opendir(const char *p)
{
    uint8_t k = classify(p);
    if (k == 1 || k == 2) evq_io_counters()->sd_read_open++;
    return opendir(p);
}

int evq_hil_ferror(FILE *f)
{
    hil_of_t *o = of_find(f);
    if (o != NULL && o->err) return 1;
    return ferror(f);
}

void evq_hil_clearerr(FILE *f)
{
    hil_of_t *o = of_find(f);
    if (o != NULL) o->err = false;
    clearerr(f);
}

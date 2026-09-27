/* HIL-only I/O wrappers + named fault points for the SD writers
 * (CONFIG_AMBYTE_EVQ_HIL). Compiled only with the flag (CMakeLists). The
 * release build never links this file; its EVQ_FAULT_POINT stays empty and
 * evq_hil_io_w.h defines nothing.
 *
 * Two arming styles share these wrappers:
 *   - legacy named points (event_log only): `evq_hil fault <point> <mode>` —
 *     reaching the point arms "the NEXT wrapped op" (evq_arm_take_io);
 *   - writer/op-targeted (`evq_hil fault io <writer> <op> <mode> [nth] [count]`)
 *     for event_log, sd_logger, ambit_ota and ambit_flash (evq_arm_io_hit).
 *
 * Truthfulness: every "reset" here is esp_rom_software_reset_system() — a CPU
 * reset. The SD card stays powered and its controller finishes (or holds) its
 * own internal program operation; this is NOT an electrical power cut and must
 * never be reported as one. No verified SD/board power switch exists. */
#include "evq_hil_io_impl.h"
#include "event_log_hil.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "evq_hil_trace.h"
#include "sd_diag.h"

/* The wrappers must call the REAL libc functions: this file does not include
 * evq_hil_io.h / evq_hil_io_w.h, so the names below are the plain ones. */

typedef struct {
    FILE    *f;
    uint8_t  kind;        /* 0 other, 1 sd record, 2 sd other, 3 index, 4 flash segment */
    uint8_t  writer;
    bool     writable;
    bool     err;
    uint32_t bytes;
    char     path[64];
} hil_of_t;

#define HIL_MAX_OPEN 24
static hil_of_t s_of[HIL_MAX_OPEN];
static evq_tr_entry_t *s_ring_mem;

/* Retained evidence of the last fired fault: survives the CPU reset it caused
 * (RTC_NOINIT), invalidated on power-on (magic/CRC; evq_hil_fault_last). */
typedef struct {
    uint32_t magic;
    uint8_t  mode, writer, op, pad;
    int32_t  err;
    uint32_t nth;
    int64_t  uptime_us;
    char     path[40];
    uint32_t crc;
} hil_last_t;
#define HIL_LAST_MAGIC 0x484C4654u   /* 'HLFT' */
static RTC_NOINIT_ATTR hil_last_t s_last;

static int64_t now_us(void) { return esp_timer_get_time(); }

static uint32_t last_crc(const hil_last_t *l)
{
    const uint8_t *p = (const uint8_t *)l;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < offsetof(hil_last_t, crc); i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

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
    if (esp_reset_reason() == ESP_RST_POWERON) memset(&s_last, 0, sizeof s_last);
}

void evq_hil_rom_reset(const char *why)
{
    printf("\r\nHIL_FAULT fired %s us=%lld sd_power=not_interrupted mechanism=esp_rom_software_reset_system\r\n",
           why ? why : "-", (long long)now_us());
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

bool evq_hil_fault_last(evq_hil_last_view_t *out)
{
    if (s_last.magic != HIL_LAST_MAGIC || s_last.crc != last_crc(&s_last)) return false;
    out->mode = s_last.mode;
    out->writer = s_last.writer;
    out->op = s_last.op;
    out->err = s_last.err;
    out->nth = s_last.nth;
    out->uptime_us = s_last.uptime_us;
    snprintf(out->path, sizeof out->path, "%s", s_last.path);
    return true;
}

/* Record + announce a fired targeted fault; reset kinds never return when
 * `reset_now`. */
static void fired(evq_iom_t m, uint8_t w, uint8_t op, const char *path, int err, unsigned nth)
{
    memset(&s_last, 0, sizeof s_last);
    s_last.magic = HIL_LAST_MAGIC;
    s_last.mode = (uint8_t)m;
    s_last.writer = w;
    s_last.op = op;
    s_last.err = err;
    s_last.nth = nth;
    s_last.uptime_us = now_us();
    if (path) {
        size_t n = strlen(path);
        snprintf(s_last.path, sizeof s_last.path, "%s", n >= sizeof s_last.path ? path + n - (sizeof s_last.path - 1) : path);
    }
    s_last.crc = last_crc(&s_last);
    bool reset = m == EVQ_IOM_RESET_BEFORE || m == EVQ_IOM_RESET_AFTER || m == EVQ_IOM_RESET_MID_WRITE;
    printf("\r\nHIL_FAULT fired kind=%s mode=%s writer=%s op=%s path=%s errno=%d nth=%u%s\r\n",
           evq_iom_kind(m), evq_iom_name(m), sd_diag_writer_name(w), sd_diag_op_name(op), path ? path : "-", err, nth,
           reset ? " sd_power=not_interrupted mechanism=esp_rom_software_reset_system" : "");
    evq_tr_record(now_us(), EVQ_TR_FAULT, -err, nth, path, evq_iom_name(m));
}

static void do_reset(void) __attribute__((noreturn));
static void do_reset(void)
{
    fflush(stdout);
    esp_rom_delay_us(60000);
    esp_rom_software_reset_system();
    for (;;) { }
}

/* Decide this op's fate. Legacy "next op" injection applies to event_log only.
 * Returns: 0 run normally; >0 inject that errno without running; or one of the
 * targeted modes via *mode (caller handles short/applied/reset). */
static int take(uint8_t w, uint8_t op, const char *path, evq_iom_t *mode, unsigned *nth)
{
    *mode = EVQ_IOM_NONE;
    if (w == SD_DIAG_W_EVLOG) {
        int legacy = evq_arm_take_io();
        if (legacy == -1) {
            /* legacy reset_inside: writes reset midway, dir ops reset before the call */
            *mode = (op == SD_DIAG_OP_WRITE) ? EVQ_IOM_RESET_MID_WRITE : EVQ_IOM_RESET_BEFORE;
            *nth = 0;
            return 0;
        }
        if (legacy > 0) return legacy;
    }
    evq_iom_t m = evq_arm_io_hit(w, op, nth);
    if (m == EVQ_IOM_EIO || m == EVQ_IOM_ENOSPC) {
        int e = m == EVQ_IOM_EIO ? EIO : ENOSPC;
        fired(m, w, op, path, e, *nth);
        return e;
    }
    if (m == EVQ_IOM_RESET_BEFORE) {
        fired(m, w, op, path, 0, *nth);
        do_reset();
    }
    *mode = m;
    return 0;
}

static void after(evq_iom_t m, uint8_t w, uint8_t op, const char *path, unsigned nth)
{
    if (m == EVQ_IOM_RESET_AFTER) {
        fired(m, w, op, path, 0, nth);
        do_reset();
    }
}

/* Applied-then-error: the op ran; report it failed. Returns -1 with errno=EIO. */
static int applied(uint8_t w, uint8_t op, const char *path, unsigned nth)
{
    fired(EVQ_IOM_APPLIED_EIO, w, op, path, EIO, nth);
    errno = EIO;
    return -1;
}

FILE *evq_hil_io_fopen_w(uint8_t w, const char *path, const char *mode)
{
    uint8_t k = classify(path);
    bool wr = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    evq_io_counters_t *c = evq_io_counters();
    if (k == 1 || k == 2) { if (wr) c->sd_write_open++; else c->sd_read_open++; }
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_OPEN, path, &m, &nth);
    if (inj > 0) {
        errno = inj;
        if (relevant(k)) evq_tr_record(now_us(), wr ? EVQ_TR_OPEN_W : EVQ_TR_OPEN_R, -inj, 0, path, "injected");
        return NULL;
    }
    FILE *f = fopen(path, mode);
    int e = f ? 0 : errno;
    if (relevant(k)) evq_tr_record(now_us(), wr ? EVQ_TR_OPEN_W : EVQ_TR_OPEN_R, -e, 0, path, NULL);
    if (f != NULL) {
        for (int i = 0; i < HIL_MAX_OPEN; i++) {
            if (s_of[i].f == NULL) {
                s_of[i].f = f;
                s_of[i].kind = k;
                s_of[i].writer = w;
                s_of[i].writable = wr;
                s_of[i].err = false;
                s_of[i].bytes = 0;
                snprintf(s_of[i].path, sizeof s_of[i].path, "%s", path);
                break;
            }
        }
    }
    after(m, w, SD_DIAG_OP_OPEN, path, nth);
    if (f == NULL) errno = e;
    return f;
}

int evq_hil_io_fclose_w(uint8_t w, FILE *f)
{
    hil_of_t *o = of_find(f);
    char path[64];
    snprintf(path, sizeof path, "%s", o ? o->path : "-");
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_CLOSE, path, &m, &nth);
    if (inj > 0) {
        /* A refused close still releases the stream (C stdio: fclose always
         * disassociates it) — callers must never touch it again. */
        (void)fclose(f);
        if (o) o->f = NULL;
        errno = inj;
        return EOF;
    }
    int rc = fclose(f);
    int e = rc == 0 ? 0 : errno;
    if (o != NULL) {
        if (relevant(o->kind)) {
            evq_tr_record(now_us(), o->writable ? EVQ_TR_CLOSE_W : EVQ_TR_CLOSE_R, -e, o->bytes, o->path, NULL);
        }
        if (o->writable && (o->kind == 1 || o->kind == 2)) evq_io_counters()->sd_mut++;
        o->f = NULL;
    }
    after(m, w, SD_DIAG_OP_CLOSE, path, nth);
    if (rc == 0 && m == EVQ_IOM_APPLIED_EIO) { (void)applied(w, SD_DIAG_OP_CLOSE, path, nth); return EOF; }
    if (rc != 0) errno = e;
    return rc;
}

size_t evq_hil_io_fwrite_w(uint8_t w, const void *p, size_t sz, size_t n, FILE *f)
{
    hil_of_t *o = of_find(f);
    const char *path = o ? o->path : "-";
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_WRITE, path, &m, &nth);
    if (m == EVQ_IOM_RESET_MID_WRITE) {
        size_t half = (sz * n) / 2;
        (void)fwrite(p, 1, half, f);
        fflush(f);
        fsync(fileno(f));
        if (nth == 0) evq_hil_rom_reset("fwrite_inside");      /* legacy reset_inside */
        fired(m, w, SD_DIAG_OP_WRITE, path, 0, nth);
        do_reset();
    }
    if (inj > 0) {
        if (o) o->err = true;
        errno = inj;
        return 0;
    }
    if (m == EVQ_IOM_SHORT) {
        size_t half = (sz * n) / 2;
        size_t got = fwrite(p, 1, half, f);
        fflush(f);
        if (o) { o->err = true; o->bytes += (uint32_t)got; }
        fired(m, w, SD_DIAG_OP_WRITE, path, EIO, nth);
        errno = EIO;
        return sz ? got / sz : 0;
    }
    size_t wr = fwrite(p, sz, n, f);
    if (o != NULL) {
        o->bytes += (uint32_t)(wr * sz);
        if (o->kind == 1 || o->kind == 2) evq_io_counters()->sd_mut++;
        if (o->kind == 4) evq_io_counters()->store_appends++;
        if (o->kind == 3 && wr > 0) {
            char pre[EVQ_TR_NAME_MAX];
            size_t mm = wr * sz < sizeof pre - 1 ? wr * sz : sizeof pre - 1;
            memcpy(pre, p, mm);
            pre[mm] = '\0';
            for (size_t i = 0; i < mm; i++) if (pre[i] == '\n' || pre[i] == '\t') pre[i] = ' ';
            evq_tr_record(now_us(), EVQ_TR_IDX_WRITE, 0, (uint32_t)(wr * sz), o->path, pre);
        }
    }
    after(m, w, SD_DIAG_OP_WRITE, path, nth);
    return wr;
}

size_t evq_hil_io_fread_w(uint8_t w, void *p, size_t sz, size_t n, FILE *f)
{
    hil_of_t *o = of_find(f);
    const char *path = o ? o->path : "-";
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_READ, path, &m, &nth);
    if (inj > 0) {
        if (o) o->err = true;
        errno = inj;
        return 0;
    }
    size_t r = fread(p, sz, n, f);
    if (o != NULL) o->bytes += (uint32_t)(r * sz);
    after(m, w, SD_DIAG_OP_READ, path, nth);
    return r;
}

int evq_hil_io_fflush_w(uint8_t w, FILE *f)
{
    hil_of_t *o = of_find(f);
    const char *path = o ? o->path : "-";
    evq_iom_t m; unsigned nth = 0;
    /* Legacy named points never injected on fflush (event_log's durability is
     * decided by fsync); keep that behaviour for them. */
    int inj = (w == SD_DIAG_W_EVLOG) ? 0 : take(w, SD_DIAG_OP_FLUSH, path, &m, &nth);
    if (w == SD_DIAG_W_EVLOG) { m = evq_arm_io_hit(w, SD_DIAG_OP_FLUSH, &nth);
        if (m == EVQ_IOM_EIO || m == EVQ_IOM_ENOSPC) { inj = m == EVQ_IOM_EIO ? EIO : ENOSPC; fired(m, w, SD_DIAG_OP_FLUSH, path, inj, nth); }
        else if (m == EVQ_IOM_RESET_BEFORE) { fired(m, w, SD_DIAG_OP_FLUSH, path, 0, nth); do_reset(); } }
    if (inj > 0) {
        if (o) o->err = true;
        errno = inj;
        return EOF;
    }
    int rc = fflush(f);
    after(m, w, SD_DIAG_OP_FLUSH, path, nth);
    return rc;
}

int evq_hil_io_fsync_w(uint8_t w, int fd)
{
    hil_of_t *o = of_find_fd(fd);
    const char *path = o ? o->path : "-";
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_FSYNC, path, &m, &nth);
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
    after(m, w, SD_DIAG_OP_FSYNC, path, nth);
    if (rc == 0 && m == EVQ_IOM_APPLIED_EIO) return applied(w, SD_DIAG_OP_FSYNC, path, nth);
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_io_ftruncate_w(uint8_t w, int fd, off_t len)
{
    hil_of_t *o = of_find_fd(fd);
    const char *path = o ? o->path : "-";
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_TRUNCATE, path, &m, &nth);
    if (inj > 0) { errno = inj; return -1; }
    int rc = ftruncate(fd, len);
    int e = rc == 0 ? 0 : errno;
    after(m, w, SD_DIAG_OP_TRUNCATE, path, nth);
    if (rc == 0 && m == EVQ_IOM_APPLIED_EIO) return applied(w, SD_DIAG_OP_TRUNCATE, path, nth);
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_io_rename_w(uint8_t w, const char *a, const char *b)
{
    uint8_t k = classify(a);
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_RENAME, a, &m, &nth);
    if (m == EVQ_IOM_RESET_BEFORE && nth == 0) evq_hil_rom_reset("rename_inside");   /* legacy reset_inside */
    if (inj > 0) { errno = inj; if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_RENAME, -inj, 0, a, b); return -1; }
    int rc = rename(a, b);
    int e = rc == 0 ? 0 : errno;
    if (relevant(k) || relevant(classify(b))) evq_tr_record(now_us(), EVQ_TR_RENAME, -e, 0, a, b);
    if (k == 1 || k == 2) evq_io_counters()->sd_mut++;
    else if (k == 3 || k == 4) evq_io_counters()->flash_mut++;
    after(m, w, SD_DIAG_OP_RENAME, a, nth);
    if (rc == 0 && m == EVQ_IOM_APPLIED_EIO) return applied(w, SD_DIAG_OP_RENAME, a, nth);
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_io_remove_w(uint8_t w, const char *p)
{
    uint8_t k = classify(p);
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_REMOVE, p, &m, &nth);
    if (m == EVQ_IOM_RESET_BEFORE && nth == 0) evq_hil_rom_reset("remove_inside");   /* legacy reset_inside */
    if (inj > 0) { errno = inj; if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_REMOVE, -inj, 0, p, "injected"); return -1; }
    int rc = remove(p);
    int e = rc == 0 ? 0 : errno;
    if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_REMOVE, -e, 0, p, NULL);
    if (k == 1 || k == 2) evq_io_counters()->sd_mut++;
    else if (k != 0) evq_io_counters()->flash_mut++;
    after(m, w, SD_DIAG_OP_REMOVE, p, nth);
    if (rc == 0 && m == EVQ_IOM_APPLIED_EIO) return applied(w, SD_DIAG_OP_REMOVE, p, nth);
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_io_mkdir_w(uint8_t w, const char *p, mode_t md)
{
    uint8_t k = classify(p);
    evq_iom_t m; unsigned nth = 0;
    int inj = take(w, SD_DIAG_OP_MKDIR, p, &m, &nth);
    if (inj > 0) { errno = inj; return -1; }
    int rc = mkdir(p, md);
    int e = rc == 0 ? 0 : errno;
    if (relevant(k)) evq_tr_record(now_us(), EVQ_TR_MKDIR, -e, 0, p, NULL);
    if ((k == 1 || k == 2) && rc == 0) evq_io_counters()->sd_mut++;
    after(m, w, SD_DIAG_OP_MKDIR, p, nth);
    if (rc != 0) errno = e;
    return rc;
}

int evq_hil_io_stat_w(uint8_t w, const char *p, struct stat *st)
{
    uint8_t k = classify(p);
    if (k == 1 || k == 2) evq_io_counters()->sd_read_open++;   /* any SD access counts */
    /* Legacy named points never failed stat; only a targeted arm can. */
    unsigned nth = 0;
    evq_iom_t m = evq_arm_io_hit(w, SD_DIAG_OP_STAT, &nth);
    if (m == EVQ_IOM_EIO || m == EVQ_IOM_ENOSPC) {
        int e = m == EVQ_IOM_EIO ? EIO : ENOSPC;
        fired(m, w, SD_DIAG_OP_STAT, p, e, nth);
        errno = e;
        return -1;
    }
    if (m == EVQ_IOM_RESET_BEFORE) { fired(m, w, SD_DIAG_OP_STAT, p, 0, nth); do_reset(); }
    int rc = stat(p, st);
    after(m, w, SD_DIAG_OP_STAT, p, nth);
    return rc;
}

DIR *evq_hil_io_opendir_w(uint8_t w, const char *p)
{
    (void)w;
    uint8_t k = classify(p);
    if (k == 1 || k == 2) evq_io_counters()->sd_read_open++;
    return opendir(p);
}

/* ── legacy event_log entry points (evq_hil_io.h) ── */
FILE  *evq_hil_fopen(const char *path, const char *mode) { return evq_hil_io_fopen_w(SD_DIAG_W_EVLOG, path, mode); }
int    evq_hil_fclose(FILE *f) { return evq_hil_io_fclose_w(SD_DIAG_W_EVLOG, f); }
size_t evq_hil_fwrite(const void *p, size_t sz, size_t n, FILE *f) { return evq_hil_io_fwrite_w(SD_DIAG_W_EVLOG, p, sz, n, f); }
size_t evq_hil_fread(void *p, size_t sz, size_t n, FILE *f) { return evq_hil_io_fread_w(SD_DIAG_W_EVLOG, p, sz, n, f); }
int    evq_hil_fflush(FILE *f) { return evq_hil_io_fflush_w(SD_DIAG_W_EVLOG, f); }
int    evq_hil_fsync(int fd) { return evq_hil_io_fsync_w(SD_DIAG_W_EVLOG, fd); }
int    evq_hil_rename(const char *a, const char *b) { return evq_hil_io_rename_w(SD_DIAG_W_EVLOG, a, b); }
int    evq_hil_remove(const char *p) { return evq_hil_io_remove_w(SD_DIAG_W_EVLOG, p); }
int    evq_hil_mkdir(const char *p, mode_t m) { return evq_hil_io_mkdir_w(SD_DIAG_W_EVLOG, p, m); }
int    evq_hil_stat(const char *p, struct stat *st) { return evq_hil_io_stat_w(SD_DIAG_W_EVLOG, p, st); }
DIR   *evq_hil_opendir(const char *p) { return evq_hil_io_opendir_w(SD_DIAG_W_EVLOG, p); }

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

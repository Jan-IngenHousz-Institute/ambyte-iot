/* Media shim implementation (see fsshim.h). Compiled WITHOUT the forced
 * include, so the calls below reach real libc. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "evq_host.h"

#define SHIM_BLOCK 4096LL

typedef enum { MED_OTHER = 0, MED_FLASH = 1, MED_SD = 2 } medium_t;

typedef struct {
    char path[200];
    long long size;
} fent_t;

typedef struct {
    fent_t *v;
    size_t n, cap;
    long long used;
} ftab_t;

static ftab_t s_tab[3];
static bool   s_scanned[3];

typedef struct {
    FILE *f;
    char path[200];
    medium_t med;
    bool writable;
    bool err;
} ofile_t;

#define MAX_OPEN 128
static ofile_t s_open[MAX_OPEN];

static int  s_journal_fd = -1;
static int  s_ops_fd = -1;
static uint64_t s_ops_count[3];

/* fault arming (set by fault.c) */
static int  s_arm_errno = 0;             /* EIO / ENOSPC for the next operation */
static bool s_arm_crash_inside = false;  /* crash midway through the next dir/write op */
static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t rng_next(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return s_rng;
}

void shim_seed(uint64_t seed) { s_rng = seed ? seed * 0x9E3779B97F4A7C15ULL : 1; }
void shim_arm_errno(int e) { s_arm_errno = e; }
void shim_arm_crash_inside(void) { s_arm_crash_inside = true; }
uint64_t shim_sd_ops(void) { return s_ops_count[MED_SD]; }
uint64_t shim_flash_ops(void) { return s_ops_count[MED_FLASH]; }

static medium_t classify(const char *p)
{
    if (p == NULL) return MED_OTHER;
    if (p[0] == '.' && p[1] == '/') p += 2;
    if (strncmp(p, "evstore", 7) == 0 && (p[7] == '/' || p[7] == '\0')) return MED_FLASH;
    if (strncmp(p, "sdcard", 6) == 0 && (p[6] == '/' || p[6] == '\0')) return MED_SD;
    return MED_OTHER;
}

static const char *norm(const char *p)
{
    return (p[0] == '.' && p[1] == '/') ? p + 2 : p;
}

static long long blocks(long long size)
{
    return ((size + SHIM_BLOCK - 1) / SHIM_BLOCK) * SHIM_BLOCK + SHIM_BLOCK;   /* data + metadata */
}

static long long capacity(medium_t m)
{
    const char *e = getenv(m == MED_FLASH ? "EVQ_FLASH_BYTES" : "EVQ_SD_BYTES");
    return e ? atoll(e) : (m == MED_FLASH ? 2LL * 1024 * 1024 : 256LL * 1024 * 1024);
}

static void ops_log(const char *fmt, ...)
{
    if (s_ops_fd < 0) {
        mkdir(".shim", 0777);
        s_ops_fd = open(".shim/ops.jsonl", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (s_ops_fd < 0) return;
    }
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\n';
    (void)!write(s_ops_fd, buf, (size_t)n);
}

void shim_mark(const char *what)
{
    ops_log("{\"op\":\"mark\",\"what\":\"%s\"}", what);
}

static void journal(const char *fmt, ...)
{
    if (s_journal_fd < 0) {
        mkdir(".shim", 0777);
        s_journal_fd = open(".shim/journal", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (s_journal_fd < 0) return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n++] = '\n';
    (void)!write(s_journal_fd, buf, (size_t)n);
}

/* ── size table ───────────────────────────────────────────────────────── */

static fent_t *tab_find(medium_t m, const char *path)
{
    ftab_t *t = &s_tab[m];
    for (size_t i = 0; i < t->n; i++) if (strcmp(t->v[i].path, path) == 0) return &t->v[i];
    return NULL;
}

static void tab_set(medium_t m, const char *path, long long size)
{
    ftab_t *t = &s_tab[m];
    fent_t *e = tab_find(m, path);
    if (e == NULL) {
        if (t->n == t->cap) {
            t->cap = t->cap ? t->cap * 2 : 256;
            t->v = realloc(t->v, t->cap * sizeof *t->v);
        }
        e = &t->v[t->n++];
        snprintf(e->path, sizeof e->path, "%s", path);
        e->size = 0;
        t->used += blocks(0);
    }
    t->used += blocks(size) - blocks(e->size);
    e->size = size;
}

static void tab_del(medium_t m, const char *path)
{
    ftab_t *t = &s_tab[m];
    for (size_t i = 0; i < t->n; i++) {
        if (strcmp(t->v[i].path, path) == 0) {
            t->used -= blocks(t->v[i].size);
            t->v[i] = t->v[--t->n];
            return;
        }
    }
}

static void scan_dir(medium_t m, const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[200];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(m, p);
        else tab_set(m, p, (long long)st.st_size);
    }
    closedir(d);
}

static void ensure_scanned(medium_t m)
{
    if (m == MED_OTHER || s_scanned[m]) return;
    s_scanned[m] = true;
    scan_dir(m, m == MED_FLASH ? "evstore" : "sdcard");
}

/* Re-derive the SD table (card swapped/filled by the driver). */
void shim_rescan(void)
{
    for (int m = 1; m < 3; m++) {
        free(s_tab[m].v);
        memset(&s_tab[m], 0, sizeof s_tab[m]);
        s_scanned[m] = false;
        ensure_scanned((medium_t)m);
    }
}

long long shim_used(int m)  { ensure_scanned((medium_t)m); return s_tab[m].used; }
long long shim_cap(int m)   { return capacity((medium_t)m); }

/* ── per-op gates ─────────────────────────────────────────────────────── */

static void crash_now(const char *why)
{
    ops_log("{\"op\":\"crash\",\"why\":\"%s\"}", why);
    evq_host_flush_manifests();
    _exit(86);
}

/* Returns non-zero errno if this op must fail. */
static int gate(medium_t m, const char *opname, const char *path)
{
    if (m != MED_OTHER) s_ops_count[m]++;
    if (m == MED_SD && !evq_sd_stub_mounted()) {
        ops_log("{\"op\":\"%s\",\"path\":\"%s\",\"fail\":\"unmounted\"}", opname, path ? norm(path) : "");
        return EIO;
    }
    if (m != MED_OTHER && s_arm_errno != 0) {
        int e = s_arm_errno;
        s_arm_errno = 0;
        ops_log("{\"op\":\"%s\",\"path\":\"%s\",\"fail\":\"injected-%d\"}", opname, path ? norm(path) : "", e);
        return e;
    }
    if (m != MED_OTHER && s_arm_crash_inside &&
        strcmp(opname, "rename") != 0 && strcmp(opname, "remove") != 0 && strcmp(opname, "fwrite") != 0) {
        /* only dir ops and writes have a meaningful "inside" state; any other
         * op simply crashes before running */
        s_arm_crash_inside = false;
        crash_now(opname);
    }
    return 0;
}

static ofile_t *of_find(FILE *f)
{
    for (int i = 0; i < MAX_OPEN; i++) if (s_open[i].f == f) return &s_open[i];
    return NULL;
}

static ofile_t *of_find_fd(int fd)
{
    for (int i = 0; i < MAX_OPEN; i++) if (s_open[i].f != NULL && fileno(s_open[i].f) == fd) return &s_open[i];
    return NULL;
}

/* ── ops ──────────────────────────────────────────────────────────────── */

FILE *shim_fopen(const char *path, const char *mode)
{
    medium_t m = classify(path);
    ensure_scanned(m);
    bool w = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    int e = gate(m, w ? "open_w" : "open_r", path);
    if (e) { errno = e; return NULL; }
    struct stat st;
    bool existed = stat(path, &st) == 0;
    if (m != MED_OTHER && w && !existed && s_tab[m].used + SHIM_BLOCK > capacity(m)) {
        ops_log("{\"op\":\"open_w\",\"path\":\"%s\",\"fail\":\"enospc\"}", norm(path));
        errno = ENOSPC;
        return NULL;
    }
    FILE *f = fopen(path, mode);
    if (f == NULL) return NULL;
    if (m != MED_OTHER) {
        for (int i = 0; i < MAX_OPEN; i++) {
            if (s_open[i].f == NULL) {
                s_open[i].f = f;
                snprintf(s_open[i].path, sizeof s_open[i].path, "%s", norm(path));
                s_open[i].med = m;
                s_open[i].writable = w;
                s_open[i].err = false;
                break;
            }
        }
        if (w) {
            fstat(fileno(f), &st);
            tab_set(m, norm(path), (long long)st.st_size);
            journal("O %d %s %lld %d", (int)m, norm(path), (long long)st.st_size, existed ? 0 : 1);
            ops_log("{\"op\":\"open_w\",\"path\":\"%s\",\"new\":%d}", norm(path), existed ? 0 : 1);
        } else if (m == MED_SD) {
            ops_log("{\"op\":\"open_r\",\"path\":\"%s\"}", norm(path));
        }
    }
    return f;
}

static void record_durable(ofile_t *o)
{
    struct stat st;
    if (fstat(fileno(o->f), &st) != 0) return;
    journal("D %d %s %lld", (int)o->med, o->path, (long long)st.st_size);
    ops_log("{\"op\":\"sync\",\"path\":\"%s\",\"size\":%lld}", o->path, (long long)st.st_size);
}

int shim_fclose(FILE *f)
{
    ofile_t *o = of_find(f);
    int rc;
    if (o != NULL) {
        if (o->writable) {
            if (o->med == MED_SD && !evq_sd_stub_mounted()) {
                /* card gone: nothing reaches it */
                o->f = NULL;
                fclose(f);
                return EOF;
            }
            fflush(f);
            record_durable(o);          /* FAT f_close and littlefs close both sync */
        }
        o->f = NULL;
    }
    rc = fclose(f);
    return rc;
}

size_t shim_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    ofile_t *o = of_find(f);
    if (o == NULL) return fwrite(p, sz, n, f);
    size_t bytes = sz * n;
    int e = gate(o->med, "fwrite", o->path);
    if (e) { o->err = true; errno = e; return 0; }
    struct stat st;
    fstat(fileno(f), &st);
    long long pos = (long long)ftell(f);
    long long newsize = pos + (long long)bytes > (long long)st.st_size ? pos + (long long)bytes : (long long)st.st_size;
    fent_t *ent = tab_find(o->med, o->path);
    long long cur = ent ? ent->size : (long long)st.st_size;
    long long delta = blocks(newsize) - blocks(cur);
    if (s_tab[o->med].used + delta > capacity(o->med)) {
        ops_log("{\"op\":\"fwrite\",\"path\":\"%s\",\"fail\":\"enospc\"}", o->path);
        o->err = true;
        errno = ENOSPC;
        return 0;
    }
    if (s_arm_crash_inside) {
        s_arm_crash_inside = false;
        size_t part = bytes ? (size_t)(rng_next() % bytes) : 0;
        fwrite(p, 1, part, f);
        fflush(f);
        crash_now("fwrite");
    }
    size_t w = fwrite(p, sz, n, f);
    fflush(f);                          /* write-through: capacity is exact */
    if (o->med == MED_FLASH && (strstr(o->path, "evq.idx") != NULL) && w > 0) {
        /* cumulative index history for the ARCH-INV oracle (survives compaction) */
        int hf = open(".shim/idx_history", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (hf >= 0) { (void)!write(hf, p, sz * w); close(hf); }
    }
    fstat(fileno(f), &st);
    tab_set(o->med, o->path, (long long)st.st_size);
    return w;
}

static int read_gate(ofile_t *o)
{
    if (o == NULL) return 0;
    /* Persistent bad sectors: every data read of the SD file that is at
     * EVQ_SHIM_BADREAD (normalized path) when the run starts fails, under
     * whatever name it has later: the damage is in its data clusters, which
     * a rename keeps (tracked by inode). Directory ops still work. */
    static int bad_state = 0;            /* 0 unresolved, 1 armed, -1 off */
    static ino_t bad_ino;
    static dev_t bad_dev;
    if (bad_state == 0) {
        const char *bad = getenv("EVQ_SHIM_BADREAD");
        struct stat bst;
        if (bad != NULL && bad[0] != '\0' && stat(bad, &bst) == 0) {
            bad_ino = bst.st_ino;
            bad_dev = bst.st_dev;
            bad_state = 1;
        } else {
            bad_state = -1;
        }
    }
    if (bad_state == 1 && o->med == MED_SD) {
        struct stat fst;
        if (fstat(fileno(o->f), &fst) == 0 && fst.st_ino == bad_ino && fst.st_dev == bad_dev) {
            ops_log("{\"op\":\"read\",\"path\":\"%s\",\"fail\":\"badread\"}", o->path);
            o->err = true;
            errno = EIO;
            return EIO;
        }
    }
    int e = gate(o->med, "read", o->path);
    if (e) { o->err = true; errno = e; }
    return e;
}

size_t shim_fread(void *p, size_t sz, size_t n, FILE *f)
{
    ofile_t *o = of_find(f);
    if (read_gate(o)) return 0;
    return fread(p, sz, n, f);
}

char *shim_fgets(char *s, int n, FILE *f)
{
    ofile_t *o = of_find(f);
    if (read_gate(o)) return NULL;
    return fgets(s, n, f);
}

int shim_fgetc(FILE *f)
{
    ofile_t *o = of_find(f);
    if (o != NULL && o->med == MED_SD && !evq_sd_stub_mounted()) { o->err = true; return EOF; }
    return fgetc(f);
}

int shim_fflush(FILE *f)
{
    ofile_t *o = of_find(f);
    if (o != NULL) {
        int e = gate(o->med, "fflush", o->path);
        if (e) { o->err = true; errno = e; return EOF; }
    }
    return fflush(f);
}

int shim_fseek(FILE *f, long off, int whence)
{
    ofile_t *o = of_find(f);
    if (o != NULL && o->med == MED_SD && !evq_sd_stub_mounted()) { o->err = true; errno = EIO; return -1; }
    return fseek(f, off, whence);
}

int shim_ferror(FILE *f)
{
    ofile_t *o = of_find(f);
    if (o != NULL && o->err) return 1;
    return ferror(f);
}

void shim_clearerr(FILE *f)
{
    ofile_t *o = of_find(f);
    if (o != NULL) o->err = false;
    clearerr(f);
}

int shim_fsync(int fd)
{
    ofile_t *o = of_find_fd(fd);
    if (o == NULL) return fsync(fd);
    int e = gate(o->med, "fsync", o->path);
    if (e) { o->err = true; errno = e; return -1; }
    fflush(o->f);
    record_durable(o);
    return 0;          /* host page cache is the "medium"; durability is the journal */
}

int shim_ftruncate(int fd, off_t len)
{
    ofile_t *o = of_find_fd(fd);
    if (o == NULL) return ftruncate(fd, len);
    int e = gate(o->med, "truncate", o->path);
    if (e) { errno = e; return -1; }
    int rc = ftruncate(fd, len);
    if (rc == 0) {
        tab_set(o->med, o->path, (long long)len);
        /* A truncate is modelled as durable at once (it only ever removes
         * never-acknowledged bytes in the production code). */
        journal("D %d %s %lld", (int)o->med, o->path, (long long)len);
        ops_log("{\"op\":\"truncate\",\"path\":\"%s\",\"size\":%lld}", o->path, (long long)len);
    }
    return rc;
}

int shim_rename(const char *a, const char *b)
{
    medium_t m = classify(a);
    ensure_scanned(m);
    int e = gate(m, "rename", a);
    if (e) { errno = e; return -1; }
    struct stat st;
    long long sz = stat(a, &st) == 0 ? (long long)st.st_size : 0;
    if (s_arm_crash_inside) {
        s_arm_crash_inside = false;
        /* littlefs rename is atomic: {old, new}. FAT f_rename between
         * dir_register and dir_remove can leave BOTH names (a cross-link on
         * real FAT; a hard link here). */
        unsigned outcome = (unsigned)(rng_next() % (m == MED_SD ? 3 : 2));
        const char *what = outcome == 0 ? "not_applied" : outcome == 1 ? "applied" : "both";
        if (outcome == 1) { if (rename(a, b) == 0) { journal("R %d %s %s", (int)m, norm(a), norm(b)); } }
        else if (outcome == 2) { if (link(a, b) == 0) journal("L %d %s %s", (int)m, norm(a), norm(b)); }
        ops_log("{\"op\":\"rename_inside\",\"from\":\"%s\",\"to\":\"%s\",\"outcome\":\"%s\"}", norm(a), norm(b), what);
        crash_now("rename");
    }
    /* never replace an existing target on SD (FatFs f_rename → FR_EXIST) */
    if (m == MED_SD && stat(b, &st) == 0) { errno = EEXIST; return -1; }
    int rc = rename(a, b);
    if (rc == 0) {
        tab_del(m, norm(a));
        tab_set(m, norm(b), sz);
        journal("R %d %s %s", (int)m, norm(a), norm(b));
        ops_log("{\"op\":\"rename\",\"from\":\"%s\",\"to\":\"%s\"}", norm(a), norm(b));
    }
    return rc;
}

int shim_remove(const char *p)
{
    medium_t m = classify(p);
    ensure_scanned(m);
    int e = gate(m, "remove", p);
    if (e) { errno = e; return -1; }
    if (s_arm_crash_inside) {
        s_arm_crash_inside = false;
        unsigned outcome = (unsigned)(rng_next() % 2);
        if (outcome == 1 && remove(p) == 0) journal("X %d %s", (int)m, norm(p));
        ops_log("{\"op\":\"remove_inside\",\"path\":\"%s\",\"outcome\":\"%s\"}", norm(p), outcome ? "applied" : "not_applied");
        crash_now("remove");
    }
    int rc = remove(p);
    if (rc == 0) {
        tab_del(m, norm(p));
        journal("X %d %s", (int)m, norm(p));
        ops_log("{\"op\":\"remove\",\"path\":\"%s\"}", norm(p));
    }
    return rc;
}

int shim_stat(const char *p, struct stat *st)
{
    medium_t m = classify(p);
    if (m != MED_OTHER) s_ops_count[m]++;
    if (m == MED_SD && !evq_sd_stub_mounted()) { errno = ENOENT; return -1; }
    return stat(p, st);
}

int shim_mkdir(const char *p, mode_t mode)
{
    medium_t m = classify(p);
    int e = gate(m, "mkdir", p);
    if (e) { errno = e; return -1; }
    int rc = mkdir(p, mode);
    if (rc == 0) ops_log("{\"op\":\"mkdir\",\"path\":\"%s\"}", norm(p));
    return rc;
}

DIR *shim_opendir(const char *p)
{
    medium_t m = classify(p);
    int e = gate(m, "opendir", p);
    if (e) { errno = e; return NULL; }
    if (m == MED_SD) ops_log("{\"op\":\"opendir\",\"path\":\"%s\"}", norm(p));
    return opendir(p);
}

struct dirent *shim_readdir(DIR *d) { return readdir(d); }
int shim_closedir(DIR *d) { return closedir(d); }

/* esp_littlefs_info for the flash medium. */
int shim_flash_info(size_t *total, size_t *used)
{
    ensure_scanned(MED_FLASH);
    if (total) *total = (size_t)capacity(MED_FLASH);
    if (used)  *used  = (size_t)(s_tab[MED_FLASH].used > capacity(MED_FLASH) ? capacity(MED_FLASH) : s_tab[MED_FLASH].used);
    return 0;
}

long long shim_sd_free(void)
{
    ensure_scanned(MED_SD);
    long long f = capacity(MED_SD) - s_tab[MED_SD].used;
    return f > 0 ? f : 0;
}

/* Q1: a purpose-built poison record's parse allocation always fails (a
 * record too big for the fragmented heap), so the claim path's OOM escape is
 * exercised on production code. Only strings carrying the marker fail. */
char *shim_strdup(const char *s)
{
    if (strstr(s, "\"oom_poison\":true") != NULL) { errno = ENOMEM; return NULL; }
    return strdup(s);
}

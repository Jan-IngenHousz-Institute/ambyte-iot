/* Host media shim implementation (see sd_shim.h). Plain libc here. */
#include "sd_shim.h"

#undef fopen
#undef fclose
#undef fwrite
#undef fread
#undef fflush
#undef fsync
#undef ftruncate
#undef fseek
#undef ftell
#undef fgetc
#undef rename
#undef remove
#undef mkdir
#undef stat
#undef opendir
#undef time

#include <stdbool.h>

typedef struct { FILE *f; char path[256]; } shim_of_t;
#define SHIM_MAX_OPEN 32
static shim_of_t s_of[SHIM_MAX_OPEN];

/* Independent arms: each counts its own matching ops (so "the 1st fsync fails
 * with EIO and the 2nd is applied-then-EIO" is two arms on fsync). */
typedef struct {
    char        op[16];
    char        sub[128];
    shim_mode_t mode;
    unsigned    nth, count, hits, fired;
} shim_arm_t;
#define SHIM_MAX_ARMS 8
static shim_arm_t  s_arms[SHIM_MAX_ARMS];
static FILE       *s_log;
static unsigned    s_viol;
static time_t      s_time = 1790000000;

struct opc { const char *op; unsigned n; };
static struct opc s_counts[] = {
    { "fopen", 0 }, { "fclose", 0 }, { "fwrite", 0 }, { "fread", 0 }, { "fflush", 0 }, { "fsync", 0 },
    { "ftruncate", 0 }, { "fseek", 0 }, { "ftell", 0 }, { "fgetc", 0 }, { "rename", 0 }, { "remove", 0 },
    { "mkdir", 0 }, { "stat", 0 }, { "opendir", 0 },
};

void (*g_shim_before_op)(const char *op, const char *path);

static bool is_sd(const char *p)
{
    if (p == NULL) return false;
    if (p[0] == '.' && p[1] == '/') p += 2;
    return strncmp(p, "sdcard", 6) == 0;
}

static const char *of_path(FILE *f)
{
    for (int i = 0; i < SHIM_MAX_OPEN; i++) if (s_of[i].f == f && f != NULL) return s_of[i].path;
    return "";
}

static const char *fd_path(int fd)
{
    for (int i = 0; i < SHIM_MAX_OPEN; i++) if (s_of[i].f != NULL && fileno(s_of[i].f) == fd) return s_of[i].path;
    return "";
}

int shim_parse_mode(const char *s, shim_mode_t *out)
{
    static const struct { const char *n; shim_mode_t m; } t[] = {
        { "eio", SHIM_M_EIO }, { "enospc", SHIM_M_ENOSPC }, { "short", SHIM_M_SHORT },
        { "applied_eio", SHIM_M_APPLIED_EIO }, { "eexist", SHIM_M_EEXIST }, { "lose", SHIM_M_LOSE },
        { "crash", SHIM_M_CRASH }, { "crash_after", SHIM_M_CRASH_AFTER }, { "crash_mid", SHIM_M_CRASH_MID },
        { "flip_read", SHIM_M_FLIP_READ }, { "off", SHIM_M_NONE },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(s, t[i].n) == 0) { *out = t[i].m; return 0; }
    return -1;
}

int shim_arm(const char *op, const char *path_sub, shim_mode_t mode, unsigned nth, unsigned count)
{
    bool known = false;
    for (size_t i = 0; i < sizeof s_counts / sizeof s_counts[0]; i++) if (strcmp(op, s_counts[i].op) == 0) known = true;
    if (!known) return -1;
    for (int i = 0; i < SHIM_MAX_ARMS; i++) {
        shim_arm_t *a = &s_arms[i];
        if (a->mode != SHIM_M_NONE) continue;
        snprintf(a->op, sizeof a->op, "%s", op);
        snprintf(a->sub, sizeof a->sub, "%s", path_sub ? path_sub : "");
        a->mode = mode;
        a->nth = nth ? nth : 1;
        a->count = count ? count : 1;
        a->hits = a->fired = 0;
        return 0;
    }
    return -2;
}

void shim_disarm(void) { memset(s_arms, 0, sizeof s_arms); }
void shim_open_log(const char *path) { s_log = fopen(path, "a"); }
unsigned shim_violations(void) { return s_viol; }
void shim_set_time(time_t t) { s_time = t; }

unsigned shim_op_count(const char *op)
{
    for (size_t i = 0; i < sizeof s_counts / sizeof s_counts[0]; i++) if (strcmp(op, s_counts[i].op) == 0) return s_counts[i].n;
    return 0;
}

static void logop(const char *op, const char *path, long ret, int err, const char *inj)
{
    if (s_log == NULL) return;
    fprintf(s_log, "{\"op\":\"%s\",\"path\":\"%s\",\"ret\":%ld,\"errno\":%d,\"refs\":%d,\"lost\":%d,\"inj\":\"%s\"}\n",
            op, path ? path : "", ret, err, g_sd_refs, g_sd_lost, inj ? inj : "");
    fflush(s_log);
}

/* Common gate: counting, ref check, fault decision. Returns the mode to apply. */
static shim_mode_t gate(const char *op, const char *path)
{
    if (g_shim_before_op) g_shim_before_op(op, path);
    for (size_t i = 0; i < sizeof s_counts / sizeof s_counts[0]; i++) if (strcmp(op, s_counts[i].op) == 0) s_counts[i].n++;
    if (is_sd(path) && g_sd_refs <= 0) {
        s_viol++;
        logop("VIOLATION", path, 0, 0, op);
    }
    shim_mode_t act = SHIM_M_NONE;
    for (int i = 0; i < SHIM_MAX_ARMS; i++) {
        shim_arm_t *a = &s_arms[i];
        if (a->mode == SHIM_M_NONE || strcmp(op, a->op) != 0 || (a->sub[0] != '\0' && strstr(path, a->sub) == NULL)) continue;
        if (++a->hits >= a->nth && act == SHIM_M_NONE) {
            act = a->mode;
            if (++a->fired >= a->count) a->mode = SHIM_M_NONE;
        }
    }
    /* A lost card fails every operation on it (like an unmounted volume). */
    if (act == SHIM_M_NONE && g_sd_lost && is_sd(path)) act = SHIM_M_EIO;
    return act;
}

static void crash(const char *op, const char *path)
{
    logop("crash", path, 0, 0, op);
    if (s_log) fclose(s_log);
    fflush(stdout);
    _exit(86);
}

/* Pre-op handling shared by every call: returns true when the op must fail
 * without running (errno set). */
static bool pre(shim_mode_t m, const char *op, const char *path)
{
    switch (m) {
    case SHIM_M_EIO:    errno = EIO;    logop(op, path, -1, EIO, "eio"); return true;
    case SHIM_M_ENOSPC: errno = ENOSPC; logop(op, path, -1, ENOSPC, "enospc"); return true;
    case SHIM_M_EEXIST: errno = EEXIST; logop(op, path, -1, EEXIST, "eexist"); return true;
    case SHIM_M_LOSE:   g_sd_lost = 1; errno = EIO; logop(op, path, -1, EIO, "lose"); return true;
    case SHIM_M_CRASH:  crash(op, path); return true;
    default: return false;
    }
}

static int post_int(shim_mode_t m, int rc, const char *op, const char *path)
{
    if (m == SHIM_M_CRASH_AFTER) crash(op, path);
    if (m == SHIM_M_APPLIED_EIO && rc == 0) { logop(op, path, -1, EIO, "applied_eio"); errno = EIO; return -1; }
    logop(op, path, rc, rc == 0 ? 0 : errno, "");
    return rc;
}

FILE *shim_fopen(const char *path, const char *mode)
{
    shim_mode_t m = gate("fopen", path);
    if (pre(m, "fopen", path)) return NULL;
    FILE *f = fopen(path, mode);
    int e = errno;
    if (f != NULL) {
        for (int i = 0; i < SHIM_MAX_OPEN; i++) if (s_of[i].f == NULL) {
            s_of[i].f = f;
            snprintf(s_of[i].path, sizeof s_of[i].path, "%s", path);
            break;
        }
    }
    if (m == SHIM_M_CRASH_AFTER) crash("fopen", path);
    logop("fopen", path, f ? 0 : -1, f ? 0 : e, mode);
    errno = e;
    return f;
}

int shim_fclose(FILE *f)
{
    char path[256];
    snprintf(path, sizeof path, "%s", of_path(f));
    shim_mode_t m = gate("fclose", path);
    for (int i = 0; i < SHIM_MAX_OPEN; i++) if (s_of[i].f == f) s_of[i].f = NULL;
    if (m == SHIM_M_EIO || m == SHIM_M_ENOSPC || m == SHIM_M_LOSE) {
        /* C stdio: a failing fclose still releases the stream */
        fclose(f);
        if (m == SHIM_M_LOSE) g_sd_lost = 1;
        errno = m == SHIM_M_ENOSPC ? ENOSPC : EIO;
        logop("fclose", path, -1, errno, "inj");
        return EOF;
    }
    if (m == SHIM_M_CRASH) crash("fclose", path);
    int rc = fclose(f);
    return post_int(m, rc, "fclose", path);
}

size_t shim_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    const char *path = of_path(f);
    shim_mode_t m = gate("fwrite", path);
    if (m == SHIM_M_SHORT || m == SHIM_M_CRASH_MID) {
        size_t half = (sz * n) / 2;
        size_t got = fwrite(p, 1, half, f);
        fflush(f);
        if (m == SHIM_M_CRASH_MID) { fsync(fileno(f)); crash("fwrite", path); }
        logop("fwrite", path, (long)got, EIO, "short");
        errno = EIO;
        return sz ? got / sz : 0;
    }
    if (pre(m, "fwrite", path)) return 0;
    size_t w = fwrite(p, sz, n, f);
    if (m == SHIM_M_CRASH_AFTER) { fflush(f); crash("fwrite", path); }
    logop("fwrite", path, (long)(w * sz), 0, "");
    return w;
}

size_t shim_fread(void *p, size_t sz, size_t n, FILE *f)
{
    const char *path = of_path(f);
    shim_mode_t m = gate("fread", path);
    if (pre(m, "fread", path)) return 0;
    size_t r = fread(p, sz, n, f);
    if (m == SHIM_M_FLIP_READ && r > 0) {
        ((uint8_t *)p)[0] ^= 0x5A;          /* the card hands back different bytes */
        logop("fread", path, (long)(r * sz), 0, "flip_read");
        return r;
    }
    if (m == SHIM_M_APPLIED_EIO) { logop("fread", path, 0, EIO, "applied_eio"); errno = EIO; return 0; }
    if (m == SHIM_M_CRASH_AFTER) crash("fread", path);
    return r;
}

int shim_fflush(FILE *f)
{
    const char *path = of_path(f);
    shim_mode_t m = gate("fflush", path);
    if (pre(m, "fflush", path)) return EOF;
    int rc = fflush(f);
    return post_int(m, rc, "fflush", path);
}

int shim_fsync(int fd)
{
    const char *path = fd_path(fd);
    shim_mode_t m = gate("fsync", path);
    if (pre(m, "fsync", path)) return -1;
    int rc = fsync(fd);
    return post_int(m, rc, "fsync", path);
}

int shim_ftruncate(int fd, off_t len)
{
    const char *path = fd_path(fd);
    shim_mode_t m = gate("ftruncate", path);
    if (pre(m, "ftruncate", path)) return -1;
    int rc = ftruncate(fd, len);
    return post_int(m, rc, "ftruncate", path);
}

int shim_fseek(FILE *f, long off, int whence)
{
    const char *path = of_path(f);
    shim_mode_t m = gate("fseek", path);
    if (pre(m, "fseek", path)) return -1;
    return fseek(f, off, whence);
}

long shim_ftell(FILE *f)
{
    const char *path = of_path(f);
    shim_mode_t m = gate("ftell", path);
    if (pre(m, "ftell", path)) return -1;
    return ftell(f);
}

int shim_fgetc(FILE *f)
{
    const char *path = of_path(f);
    shim_mode_t m = gate("fgetc", path);
    if (pre(m, "fgetc", path)) return EOF;
    return fgetc(f);
}

int shim_rename(const char *a, const char *b)
{
    shim_mode_t m = gate("rename", a);
    if (pre(m, "rename", a)) return -1;
    struct stat st;
    if (is_sd(a) && stat(b, &st) == 0) {        /* FatFs f_rename never replaces: FR_EXIST */
        errno = EEXIST;
        logop("rename", a, -1, EEXIST, b);
        return -1;
    }
    int rc = rename(a, b);
    if (m == SHIM_M_CRASH_AFTER) crash("rename", a);
    if (m == SHIM_M_APPLIED_EIO && rc == 0) { logop("rename", a, -1, EIO, "applied_eio"); errno = EIO; return -1; }
    logop("rename", a, rc, rc == 0 ? 0 : errno, b);
    return rc;
}

int shim_remove(const char *p)
{
    shim_mode_t m = gate("remove", p);
    if (pre(m, "remove", p)) return -1;
    int rc = remove(p);
    return post_int(m, rc, "remove", p);
}

int shim_mkdir(const char *p, mode_t md)
{
    shim_mode_t m = gate("mkdir", p);
    if (pre(m, "mkdir", p)) return -1;
    int rc = mkdir(p, md);
    return post_int(m, rc, "mkdir", p);
}

int shim_stat(const char *p, struct stat *st)
{
    shim_mode_t m = gate("stat", p);
    if (pre(m, "stat", p)) return -1;
    return stat(p, st);
}

DIR *shim_opendir(const char *p)
{
    shim_mode_t m = gate("opendir", p);
    if (pre(m, "opendir", p)) return NULL;
    DIR *d = opendir(p);
    logop("opendir", p, d ? 0 : -1, d ? 0 : errno, "");
    return d;
}

time_t shim_time(time_t *t)
{
    if (t) *t = s_time;
    return s_time;
}

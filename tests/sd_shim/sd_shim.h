/* Forced-include media shim for the PRODUCTION SD writers under host test
 * (tests/sdlog_host: sd_logger.c; tests/ambit_host: ambit_stage.c,
 * ambit_flash_preflight.c and the sliced baseline functions).
 *
 * Every file operation of the translation unit is routed through shim_* so the
 * harness can:
 *   - give "./sdcard" FAT rename semantics (never replace an existing target:
 *     EEXIST, like FatFs f_rename → FR_EXIST);
 *   - inject a fault on the nth matching op: eio | enospc | short (writes
 *     floor(n/2), returns it, errno=EIO) | applied_eio (op performed, then -1/EIO)
 *     | eexist | lose (EIO + latch the SD loss flag) | crash (process exits 86
 *     BEFORE the op — a simulated CPU reset; state on disk is whatever was
 *     already written) | crash_after (exit 86 after the op) | crash_mid (write:
 *     half written + flushed + synced, then exit 86);
 *   - log every SD op with the sdcard io-ref count held at call time (ops.jsonl),
 *     so tests can prove no FATFS call happens outside sdcard_io_begin/end.
 * Only the production TUs are compiled with -include sd_shim.h; the shim and
 * the drivers use plain libc. */
#ifndef SD_SHIM_H
#define SD_SHIM_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

FILE  *shim_fopen(const char *path, const char *mode);
int    shim_fclose(FILE *f);
size_t shim_fwrite(const void *p, size_t sz, size_t n, FILE *f);
size_t shim_fread(void *p, size_t sz, size_t n, FILE *f);
int    shim_fflush(FILE *f);
int    shim_fsync(int fd);
int    shim_ftruncate(int fd, off_t len);
int    shim_fseek(FILE *f, long off, int whence);
long   shim_ftell(FILE *f);
int    shim_fgetc(FILE *f);
int    shim_rename(const char *a, const char *b);
int    shim_remove(const char *p);
int    shim_mkdir(const char *p, mode_t m);
int    shim_stat(const char *p, struct stat *st);
DIR   *shim_opendir(const char *p);
time_t shim_time(time_t *t);

#define fopen(p, m)          shim_fopen(p, m)
#define fclose(f)            shim_fclose(f)
#define fwrite(p, s, n, f)   shim_fwrite(p, s, n, f)
#define fread(p, s, n, f)    shim_fread(p, s, n, f)
#define fflush(f)            shim_fflush(f)
#define fsync(fd)            shim_fsync(fd)
#define ftruncate(fd, l)     shim_ftruncate(fd, l)
#define fseek(f, o, w)       shim_fseek(f, o, w)
#define ftell(f)             shim_ftell(f)
#define fgetc(f)             shim_fgetc(f)
#define rename(a, b)         shim_rename(a, b)
#define remove(p)            shim_remove(p)
#define mkdir(p, m)          shim_mkdir(p, m)
#define stat(p, st)          shim_stat(p, st)
#define opendir(p)           shim_opendir(p)
#define time(t)              shim_time(t)

/* ── harness control (driver side) ── */
typedef enum {
    SHIM_M_NONE = 0, SHIM_M_EIO, SHIM_M_ENOSPC, SHIM_M_SHORT, SHIM_M_APPLIED_EIO, SHIM_M_EEXIST,
    SHIM_M_LOSE, SHIM_M_CRASH, SHIM_M_CRASH_AFTER, SHIM_M_CRASH_MID, SHIM_M_FLIP_READ,
} shim_mode_t;

int  shim_parse_mode(const char *s, shim_mode_t *out);
/* op: fopen fclose fwrite fread fflush fsync ftruncate fseek ftell fgetc rename
 * remove mkdir stat opendir; `path_sub` (may be NULL) restricts matching to
 * paths containing it (fd/FILE ops match the path they were opened with). */
int  shim_arm(const char *op, const char *path_sub, shim_mode_t mode, unsigned nth, unsigned count);
void shim_disarm(void);
void shim_open_log(const char *path);
unsigned shim_violations(void);      /* SD ops observed with io refs == 0 */
unsigned shim_op_count(const char *op);
void shim_set_time(time_t t);

/* sdcard stub state (sd_host_stubs.c) */
extern int  g_sd_refs;
extern int  g_sd_lost;
extern int  g_sd_mounted;
extern int  g_sd_teardown;
extern unsigned g_sd_io_errors, g_sd_io_oks;
/* hook: called before every shim op (op name, path) — used to raise a
 * teardown at a chosen boundary. */
extern void (*g_shim_before_op)(const char *op, const char *path);

#endif

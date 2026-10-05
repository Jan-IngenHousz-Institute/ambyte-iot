/* Forced-include media shim for the production event_log.c / evq_index.c
 * (tests/evq_host). Every file operation those translation units make is
 * routed through shim_* so the harness can:
 *   - classify each path as FLASH (./evstore…) or SD (./sdcard…),
 *   - enforce per-medium capacity (ENOSPC) with block rounding,
 *   - inject EIO / ENOSPC on the next operation after an armed fault point,
 *   - fail every SD operation while the card is "removed",
 *   - record each file's DURABLE size (last fsync/fclose) and every
 *     directory operation's durability event (successful return) in a journal
 *     the Python power-loss transform consumes,
 *   - crash (simulated power loss) INSIDE a rename/remove/write with a seeded
 *     outcome ({not applied, applied, both names} for rename).
 * Only these two production TUs are compiled with -include fsshim.h; the
 * stubs and the driver use plain libc. */
#ifndef EVQ_FSSHIM_H
#define EVQ_FSSHIM_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

FILE  *shim_fopen(const char *path, const char *mode);
int    shim_fclose(FILE *f);
size_t shim_fwrite(const void *p, size_t sz, size_t n, FILE *f);
size_t shim_fread(void *p, size_t sz, size_t n, FILE *f);
char  *shim_fgets(char *s, int n, FILE *f);
int    shim_fgetc(FILE *f);
int    shim_fflush(FILE *f);
int    shim_fseek(FILE *f, long off, int whence);
int    shim_fsync(int fd);
int    shim_ftruncate(int fd, off_t len);
int    shim_rename(const char *a, const char *b);
int    shim_remove(const char *p);
int    shim_stat(const char *p, struct stat *st);
int    shim_mkdir(const char *p, mode_t m);
DIR   *shim_opendir(const char *p);
struct dirent *shim_readdir(DIR *d);
int    shim_closedir(DIR *d);
int    shim_ferror(FILE *f);
void   shim_clearerr(FILE *f);
char  *shim_strdup(const char *s);

#define fopen(p, m)          shim_fopen(p, m)
#define fclose(f)            shim_fclose(f)
#define fwrite(p, s, n, f)   shim_fwrite(p, s, n, f)
#define fread(p, s, n, f)    shim_fread(p, s, n, f)
#define fgets(s, n, f)       shim_fgets(s, n, f)
#define fgetc(f)             shim_fgetc(f)
#define fflush(f)            shim_fflush(f)
#define fseek(f, o, w)       shim_fseek(f, o, w)
#define fsync(fd)            shim_fsync(fd)
#define ftruncate(fd, l)     shim_ftruncate(fd, l)
#define rename(a, b)         shim_rename(a, b)
#define remove(p)            shim_remove(p)
#define stat(p, st)          shim_stat(p, st)
#define mkdir(p, m)          shim_mkdir(p, m)
#define opendir(p)           shim_opendir(p)
#define readdir(d)           shim_readdir(d)
#define closedir(d)          shim_closedir(d)
#define ferror(f)            shim_ferror(f)
#define clearerr(f)          shim_clearerr(f)
#define strdup(s)            shim_strdup(s)

#endif

/* libc error boundaries for the production durable store. One-shot failures
 * apply only to calls that the linked store actually makes. Reopen in a new
 * process to check that an error ACK cannot advance the acknowledged prefix. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned calls;
static int fail(const char *operation)
{
    const char *selected=getenv("SVC_IO_OP");
    if(!selected || strcmp(selected,operation)) return 0;
    unsigned at=getenv("SVC_IO_AT")?(unsigned)strtoul(getenv("SVC_IO_AT"),NULL,10):1;
    if(++calls!=at) return 0;
    errno=getenv("SVC_IO_EIO")?EIO:ENOSPC; return 1;
}
FILE *__real_fopen(const char *,const char *);
FILE *__wrap_fopen(const char *path,const char *mode)
{ return (strchr(mode,'w') || strchr(mode,'+')) && fail("open")?NULL:__real_fopen(path,mode); }
size_t __real_fwrite(const void *,size_t,size_t,FILE *);
size_t __wrap_fwrite(const void *p,size_t size,size_t n,FILE *f)
{
    if(fail("write")) { int e=errno;size_t done=__real_fwrite(p,size,n/2,f);errno=e;return done; }
    return __real_fwrite(p,size,n,f);
}
int __real_fflush(FILE *);
int __wrap_fflush(FILE *f)
{ if(fail("flush")) { int e=errno;(void)__real_fflush(f);errno=e;return -1; } return __real_fflush(f); }
int __real_fsync(int);
int __wrap_fsync(int fd) { return fail("sync")?-1:__real_fsync(fd); }
int __real_fclose(FILE *);
int __wrap_fclose(FILE *f)
{ if(fail("close")) { int e=errno;(void)__real_fclose(f);errno=e;return -1; } return __real_fclose(f); }
int __real_rename(const char *,const char *);
int __wrap_rename(const char *a,const char *b) { return fail("rename")?-1:__real_rename(a,b); }
int __real_unlink(const char *);
int __wrap_unlink(const char *p) { return fail("unlink")?-1:__real_unlink(p); }
int __real_ftruncate(int,off_t);
int __wrap_ftruncate(int fd,off_t n) { return fail("truncate")?-1:__real_ftruncate(fd,n); }
int __real_fseek(FILE *,long,int);
int __wrap_fseek(FILE *f,long off,int where) { return fail("seek")?-1:__real_fseek(f,off,where); }

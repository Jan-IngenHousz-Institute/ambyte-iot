#include "ota_policy.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *failure;
static bool fired;
#define FAIL(name) (!fired && failure && !strcmp(failure,name) && (fired=true))
FILE *__real_fopen(const char *, const char *);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fflush(FILE *);
int __real_fsync(int);
int __real_fclose(FILE *);
int __real_rename(const char *, const char *);
FILE *__wrap_fopen(const char *p, const char *m)
{ if (FAIL("open")) {errno=ENOSPC;return NULL;} return __real_fopen(p,m); }
size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f)
{ if (FAIL("write")) {errno=ENOSPC;return __real_fwrite(p,s,n/2,f);} return __real_fwrite(p,s,n,f); }
int __wrap_fflush(FILE *f)
{ if (FAIL("flush")) {errno=ENOSPC;return EOF;} return __real_fflush(f); }
int __wrap_fsync(int fd)
{ if (FAIL("fsync")) {errno=ENOSPC;return -1;} return __real_fsync(fd); }
int __wrap_fclose(FILE *f)
{ int r=__real_fclose(f); if(FAIL("close")){errno=ENOSPC;return EOF;} return r; }
int __wrap_rename(const char *a,const char *b)
{ if(FAIL("rename")){errno=ENOSPC;return -1;} return __real_rename(a,b); }
const esp_partition_t *esp_ota_get_running_partition(void) { return NULL; }
bool ota_policy_read_manifest(const esp_partition_t *p, ota_manifest_t *m)
{ (void)p; *m=(ota_manifest_t){2,1,1};return true; }

int main(int argc,char **argv)
{
    if(argc < 2) return 2;
    if(!strcmp(argv[1],"read")){ota_policy_print(stdout);return 0;}
    failure=argc>2?argv[2]:NULL;
    bool ok=ota_refusal_save("id\"with\\escapes","https",argv[1]);
    printf("%s\n",ok?"saved":"failed");
    return 0;
}

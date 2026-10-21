/* Observe real parser allocations before any release can hide a peak. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sched_spec.h"
static struct { void *p; size_t n; } live[256];
static size_t bytes,peak,observed_peak;
static unsigned allocations,samples;
static bool tracking,pending;
static void add(void *p,size_t n)
{
    if(!tracking || !p) return;
    assert(!pending); for(unsigned i=0;i<256;++i) if(!live[i].p) {
        live[i].p=p;live[i].n=n;bytes+=n;if(bytes>peak)peak=bytes;++allocations;pending=true;return;
    }
    abort();
}
static void drop(void *p)
{
    if(!tracking || !p) return;
    assert(!pending);for(unsigned i=0;i<256;++i)if(live[i].p==p) { bytes-=live[i].n;live[i].p=NULL;return; }
    abort();
}
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) { void *p=__real_malloc(n);add(p,n);return p; }
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t m) { void *p=__real_calloc(n,m);add(p,n*m);return p; }
void *__real_realloc(void *,size_t);
void *__wrap_realloc(void *p,size_t n) { drop(p);p=__real_realloc(p,n);add(p,n);return p; }
void __real_free(void *);
void __wrap_free(void *p) { drop(p);__real_free(p); }
void sched_resource_sample(void)
{ if(tracking) { ++samples;if(bytes>observed_peak)observed_peak=bytes;pending=false; } }
static sched_program_t program;
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);char text[16385];size_t n=fread(text,1,sizeof text,f);assert(!fclose(f));
    char error[256];tracking=true;
    assert(sched_compile_text(text,n,&program,error,sizeof error)==ESP_OK);
    assert(!bytes && !pending && peak==observed_peak && peak>=8192 && allocations>=3 && samples>=allocations);
    printf("COMPILER_METRICS_OK peak=%zu allocations=%u samples=%u\n",peak,allocations,samples);
}

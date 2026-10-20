/* Unmodified pinned SDK driver + VFS, real service wrappers/session and SD tee.
 * Only peripheral HAL, RTOS primitives and the libc->VFS binding are models.
 * The byte ring wraps and retains borrowed memory until ReturnItem. It is NOT
 * a substitute for the bench's ISR/concurrency and UART-loss qualification. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svc_usb.h"
#include "svc_session.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "hal/usb_serial_jtag_ll.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_intr_alloc.h"
#include "linenoise/linenoise.h"

struct svc_test_ring {
    unsigned char *data; size_t cap,head,tail,used,borrowed; bool usb_rx;
};
static RingbufHandle_t rx_ring;
static unsigned rings_created,critical;
static uintptr_t task=1;
static int64_t clock_us;
static uint32_t status,enabled;
static unsigned char fifo[64]; static size_t fifo_n;
static char output[65536]; static size_t output_n;
static void (*isr)(void *); static void *isr_arg;
static bool pumping;
extern void sdk_sd_log(const char *, ...);
extern void sdk_sd_check(void);
void host_crit_enter(void) { assert(!critical++); }
void host_crit_exit(void) { assert(critical--==1); }
void svc_test_enter(void) { host_crit_enter(); }
void svc_test_exit(void) { host_crit_exit(); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)task; }
int64_t esp_timer_get_time(void) { return clock_us+=100; }
void vTaskDelay(TickType_t t) { clock_us+=(int64_t)t*1000; }
void vTaskSetTimeOutState(TimeOut_t *s) { *s=0; }
BaseType_t xTaskCheckForTimeOut(TimeOut_t *s,TickType_t *t) { (void)s;(void)t; return pdFALSE; }
void *heap_caps_calloc(size_t a,size_t b,unsigned c) { (void)c; return calloc(a,b); }
void heap_caps_free(void *p) { free(p); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { unsigned *p=malloc(sizeof *p); *p=1; return p; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { unsigned *p=xSemaphoreCreateMutex(); *p=0; return p; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h,TickType_t t)
{ unsigned *p=h; (void)t; if (!*p) return pdFALSE; --*p; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { *(unsigned *)h=1; return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t h,BaseType_t *w) { (void)w; return xSemaphoreGive(h); }
void vSemaphoreDelete(SemaphoreHandle_t h) { free(h); }
RingbufHandle_t xRingbufferCreate(size_t n,RingbufferType_t type)
{
    assert(type==RINGBUF_TYPE_BYTEBUF); RingbufHandle_t r=calloc(1,sizeof *r);
    r->data=calloc(1,n); r->cap=n; if (!rings_created++) { rx_ring=r; r->usb_rx=true; } return r;
}
BaseType_t xRingbufferSendFromISR(RingbufHandle_t r,const void *p,size_t n,BaseType_t *w)
{
    (void)w; assert(!critical); if (n>r->cap-r->used) return pdFALSE;
    for (size_t i=0;i<n;++i) { r->data[r->head]=((const unsigned char *)p)[i]; r->head=(r->head+1)%r->cap; }
    r->used+=n; return pdTRUE;
}
BaseType_t xRingbufferSend(RingbufHandle_t r,const void *p,size_t n,TickType_t t)
{ (void)t; return xRingbufferSendFromISR(r,p,n,NULL); }
void *xRingbufferReceiveUpTo(RingbufHandle_t r,size_t *n,TickType_t t,size_t max)
{
    (void)t; assert(!critical && !r->borrowed); *n=r->used;
    if (*n>r->cap-r->tail) *n=r->cap-r->tail; if (*n>max) *n=max;
    r->borrowed=*n; return *n?r->data+r->tail:NULL;
}
void *xRingbufferReceiveUpToFromISR(RingbufHandle_t r,size_t *n,size_t max)
{ return xRingbufferReceiveUpTo(r,n,0,max); }
void vRingbufferReturnItem(RingbufHandle_t r,void *p)
{
    assert(p==r->data+r->tail && r->borrowed && !critical);
    if (r->usb_rx) for(size_t i=0;i<r->borrowed;++i) assert(!((unsigned char *)p)[i]);
    r->tail=(r->tail+r->borrowed)%r->cap; r->used-=r->borrowed; r->borrowed=0;
}
void vRingbufferReturnItemFromISR(RingbufHandle_t r,void *p,BaseType_t *w)
{ (void)w; vRingbufferReturnItem(r,p); }
void vRingbufferGetInfo(RingbufHandle_t r,UBaseType_t *a,UBaseType_t *b,UBaseType_t *c,UBaseType_t *d,UBaseType_t *n)
{ (void)a;(void)b;(void)c;(void)d; *n=(UBaseType_t)r->used; }
size_t xRingbufferGetCurFreeSize(RingbufHandle_t r) { return r->cap-r->used; }
void vRingbufferDelete(RingbufHandle_t r) { free(r->data); free(r); }
esp_err_t esp_intr_alloc(int source,int flags,void (*fn)(void *),void *arg,intr_handle_t *out)
{ (void)source;(void)flags; isr=fn;isr_arg=arg; *out=(void *)1; return ESP_OK; }
void esp_intr_free(intr_handle_t h) { assert(h==(void *)1); }
static void pump(void)
{
    if(pumping || !isr) return; pumping=true;
    while(enabled&USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY) { status|=USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY; isr(isr_arg); }
    pumping=false;
}
uint32_t usb_serial_jtag_ll_get_intsts_mask(void) { return status&enabled; }
void usb_serial_jtag_ll_clr_intsts_mask(uint32_t mask) { status&=~mask; }
int usb_serial_jtag_ll_txfifo_writable(void) { return 1; }
uint32_t usb_serial_jtag_ll_read_rxfifo(void *p,uint32_t max)
{ assert(fifo_n<=max); memcpy(p,fifo,fifo_n); size_t n=fifo_n; fifo_n=0; return (uint32_t)n; }
uint32_t usb_serial_jtag_ll_write_txfifo(const void *p,uint32_t n)
{ assert(output_n+n<sizeof output); memcpy(output+output_n,p,n); output_n+=n; return n; }
void usb_serial_jtag_ll_txfifo_flush(void) {}
void usb_serial_jtag_ll_ena_intr_mask(uint32_t mask) { enabled|=mask; pump(); }
void usb_serial_jtag_ll_disable_intr_mask(uint32_t mask) { enabled&=~mask; }
void usb_serial_jtag_ll_enable_bus_clock(bool e) { assert(e); }
void usb_serial_jtag_ll_phy_set_defaults(void) {}
esp_err_t esp_vfs_register_fs(const char *path,const esp_vfs_fs_ops_t *ops,int flags,void *ctx)
{ (void)path;(void)ops;(void)flags;(void)ctx; return ESP_OK; }
void linenoiseSetReadFunction(linenoise_read_bytes_fn fn) { (void)fn; }
static void inject(const void *bytes,size_t n)
{
    const unsigned char *p=bytes;
    while(n) {
        size_t k=n>64?64:n; assert(!fifo_n); memcpy(fifo,p,k); fifo_n=k;
        status|=USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT; isr(isr_arg);
        assert(!fifo_n);p+=k;n-=k;
    }
}
static ssize_t cookie_write(void *ctx,const char *bytes,size_t n)
{ (void)ctx; return esp_vfs_usb_serial_jtag_get_vfs()->write(0,bytes,n); }
static void emit_frame(uint32_t id,const char *body)
{ char frame[SVC_FRAME_MAX+1]; size_t n=svc_frame(frame,sizeof frame,id,body);assert(n && svc_usb_write(frame,n)); }
static size_t available(void *ctx) { (void)ctx; return 1024*1024; }
void svc_store_fault(const char *p) { (void)p; }
static unsigned reads;
static void engine_lock(void *c) { (void)c; svc_test_enter(); }
static void engine_unlock(void *c) { (void)c; svc_test_exit(); }
static bool enter(void *c) { (void)c;return svc_usb_enter(); }
static void leave(void *c) { (void)c;svc_usb_leave(); }
static bool write_frame(void *c,const void *p,size_t n) { (void)c; return svc_usb_write(p,n); }
static bool overrun(void *c) { (void)c;return svc_usb_take_overrun(); }
static int64_t now_ms(void *c) { (void)c;return esp_timer_get_time()/1000; }
static int read_frame(void *c,void *p,size_t n,unsigned t)
{
    (void)c;for(size_t i=0;i<n;++i) assert(!((unsigned char *)p)[i]);
    if(!reads++) {
        /* Twenty maximum lines arrive without giving the service a timeslice. */
        char data[193],body[248],line[249]; memset(data,'A',192);data[192]=0;
        snprintf(body,sizeof body,"p 0000000000000001 00000000 8bff08f2 %s",data);
        size_t length=svc_frame(line,sizeof line,1,body);assert(length==248);
        for(unsigned i=0;i<20;++i) inject(line,length);
    } else if(reads==10) {
        char line[128]; size_t k=svc_frame(line,sizeof line,99,"svc.version");inject(line,k);
    } else if(reads==11) {
        char line[128];size_t k=svc_frame(line,sizeof line,100,"svc.exit");inject(line,k);
    }
    int got=svc_usb_read(p,n,t); if(!got) clock_us+=(int64_t)t*1000;return got;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    usb_serial_jtag_driver_config_t config={256,256};assert(usb_serial_jtag_driver_install(&config)==ESP_OK);
    usb_serial_jtag_vfs_use_driver();usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    /* Four AMBIT-like byte rings: 40000 wrapped payload bytes each, no loss,
     * no wiping, and the original Ringbuf API return values on full queues. */
    for(unsigned channel=0;channel<4;++channel) {
        RingbufHandle_t r=xRingbufferCreate(256,RINGBUF_TYPE_BYTEBUF);
        unsigned char data[200];for(size_t i=0;i<sizeof data;++i)data[i]=(unsigned char)(i+channel);
        for(unsigned iteration=0;iteration<200;++iteration) {
            assert(xRingbufferSendFromISR(r,data,sizeof data,NULL));
            assert(!xRingbufferSendFromISR(r,data,sizeof data,NULL));
            size_t off=0;while(off<sizeof data) {
                size_t n;void *p=xRingbufferReceiveUpTo(r,&n,0,64);assert(n && !memcmp(p,data+off,n));
                unsigned char saved[64];memcpy(saved,p,n);vRingbufferReturnItem(r,p);assert(!memcmp(saved,p,n));off+=n;
            }
        }
        vRingbufferDelete(r);
    }
    assert(!svc_usb_take_overrun());
    const char secret[]="raw-password-and-base64-sentinel";inject(secret,sizeof secret);
    unsigned char bytes[64];assert(svc_usb_read(bytes,sizeof bytes,0)==sizeof secret && !memcmp(bytes,secret,sizeof secret));
    for(size_t i=0;i<sizeof secret;++i)assert(!rx_ring->data[i]);
    FILE *saved_stdout=stdout,*saved_stderr=stderr;
    cookie_io_functions_t funcs={.write=cookie_write};FILE *stream=fopencookie(NULL,"w",funcs);assert(stream);setvbuf(stream,NULL,_IONBF,0);
    stdout=stderr=stream;
    printf("plain-before\n");assert(output_n==13 && !memcmp(output,"plain-before\n",13));output_n=0;
    assert(svc_usb_enter());emit_frame(0,"{\"state\":\"ready\"}");
    for(unsigned i=1;i<=100;++i) {
        task=i%4+2; printf("printf sentinel %u\n",i);puts("puts sentinel");fprintf(stderr,"stderr sentinel %u\n",i);
        assert(usb_serial_jtag_write_bytes(secret,sizeof secret,0)==sizeof secret);
        if(i==1) { sdk_sd_log("W (0) sync_runner: %s\n","sentinel");sdk_sd_check(); }
        emit_frame(i,"{\"ok\":true}");
    }
    emit_frame(101,"{\"state\":\"exit\"}");svc_usb_leave();
    stdout=saved_stdout;stderr=saved_stderr;fclose(stream);task=1;
    assert(fwrite(output,1,output_n,stdout)==output_n);output_n=0;
    uint32_t dropped_calls,dropped_bytes;svc_usb_counters(&dropped_calls,&dropped_bytes);
    assert(dropped_calls>=400 && dropped_bytes>=100*sizeof secret);
    svc_store_t store;assert(!svc_store_open(&store,argv[1],1,available,NULL));
    svc_engine_t engine;svc_engine_ports_t ports={.lock=engine_lock,.unlock=engine_unlock};svc_engine_init(&engine,&store,&ports);
    svc_session_ports_t session={.enter=enter,.leave=leave,.read=read_frame,.write=write_frame,.overrun=overrun,.now_ms=now_ms};
    assert(svc_session_run(&engine,&session));assert(!store.highest && !rx_ring->used);
    assert(fwrite(output,1,output_n,stdout)==output_n);
    assert(usb_serial_jtag_driver_uninstall()==ESP_OK && !critical);
    fprintf(stderr,"PINNED_SDK_OK frames=102 uart_bytes=160000 no_store_admission=1\n");
}

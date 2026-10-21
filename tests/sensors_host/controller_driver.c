/* Complete production controller included only to seed its private composition. */
#ifndef COMMANDS_SOURCE
#define COMMANDS_SOURCE "../../components/device_commands/device_commands.c"
#endif
#include COMMANDS_SOURCE
#include "ambit_trace.h"
#include "ambit_announcement_nvs.h"
#include "nvs.h"
#include <assert.h>

static int64_t virtual_us;
static unsigned events, setters, commits, failed_stores;
static bool query_fail, fail_store;
#ifndef A3_REFERENCE
static unsigned refresh_reads;
static bool fail_cal;
#endif
static uint8_t fw[48], cal[140];
static char tuples[4][64];
static int64_t next_event;
void h_log(char l, const char *t, const char *f, ...) { (void)l; (void)t; (void)f; }
void h_enter_critical(portMUX_TYPE *m) { (void)m; }
void h_exit_critical(portMUX_TYPE *m) { (void)m; }
int64_t esp_timer_get_time(void) { return virtual_us; }
uint32_t esp_random(void) { static uint32_t seed = 7; return ++seed; }
void vTaskDelay(TickType_t t) { virtual_us += (int64_t)t * portTICK_PERIOD_MS * 1000; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h) { (void)h; return ESP_OK; }
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h) { (void)h; return ESP_OK; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "host_error"; }
void clock_trust_get_status(const char **source, bool *suspect) { *source = "rtc"; *suspect = false; }
size_t heap_caps_get_free_size(uint32_t c) { (void)c; return 100000; }
size_t heap_caps_get_largest_free_block(uint32_t c) { (void)c; return 100000; }
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *b, uint32_t n)
{ crc = ~crc; for (uint32_t i=0;i<n;++i) { crc ^= b[i]; for (unsigned j=0;j<8;++j) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); } return ~crc; }
int __wrap_gettimeofday(struct timeval *tv, void *tz)
{ (void)tz; tv->tv_sec=1792531200; tv->tv_usec=0; return 0; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{ assert(!strcmp(ns,"ambit_ann")); (void)mode; *h=1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *out,size_t *n)
{ (void)h; unsigned i=(unsigned)(k[1]-'0'); assert(i<4); if (!tuples[i][0]) return ESP_ERR_NOT_FOUND; assert(*n>strlen(tuples[i])); strcpy(out,tuples[i]); return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *value)
{ (void)h; ++setters; strcpy(tuples[(unsigned)(k[1]-'0')],value); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; ++commits; return ESP_OK; }
static esp_err_t next_id(int64_t *out) { *out=++next_event; return ESP_OK; }
static esp_err_t store(const measurement_event_desc_t *d)
{
    if (!strcmp(d->tag, MEASUREMENT_TAG_DEVICE_INFO)) {
        if (fail_store) { ++failed_stores; return ESP_FAIL; }
        ++events;
    } else {
        printf("TRACE %lld %s %s %s\n",(long long)d->measure_id,d->cmd_raw,
            d->metadata_json ? d->metadata_json : "null",d->payload_json);
    }
    return ESP_OK;
}
static esp_err_t query(uint8_t ch,const uint8_t cmd[8],const uint8_t *extra,size_t extra_n,size_t expected,uart_sensor_response_t *r,uint32_t timeout)
{
    (void)ch;(void)extra;(void)extra_n;(void)timeout;
    if (query_fail) return ESP_ERR_TIMEOUT;
    if (cmd[0]==33) {
        r->raw=malloc(expected); r->raw_len=expected;
        memcpy(r->raw,cmd[1]==2 ? fw : cal,expected);
    } else if (expected != UART_QUERY_ACK_ONLY && expected) {
        r->raw=calloc(1,expected); r->raw_len=expected;
    }
    return ESP_OK;
}
#ifndef A3_REFERENCE
static sensor_read_result_t inspect(uint8_t ch,uint8_t type,void *out,size_t n,size_t *got,uint32_t timeout,void (*started)(uint8_t))
{
    (void)timeout; started(ch); *got=0;
    if (ch) return SENSOR_READ_ABSENT;
    if (type==2) ++refresh_reads;
    if (type==1 && fail_cal) return SENSOR_READ_FAILED;
    if (type==4) return SENSOR_READ_UNSUPPORTED;
    if (type) { memcpy(out,type==2 ? fw : cal,n); *got=n; }
    return SENSOR_READ_OK;
}
static void revalidate(void)
{
    unsigned before=refresh_reads;
    do {
        uint32_t wait=sensor_inventory_wait_ms(); assert(wait!=UINT32_MAX);
        virtual_us+=(int64_t)wait*1000; sensor_inventory_refresh();
    } while (refresh_reads==before || !sensor_inventory_wait_ms());
    /* Finish calibration after its FW refresh; other ports may intervene. */
    for (unsigned i=0;i<12;++i) {
        uint32_t wait=sensor_inventory_wait_ms(); if (wait>1000) break;
        virtual_us+=(int64_t)wait*1000; sensor_inventory_refresh();
    }
    ambit_device_info_t info; assert(cmd_ambit_device_info(0,&info).status==ESP_OK);
}
#endif
static void trace(void)
{
    uart_sensor_response_t r={.array_count=2};
    for (unsigned i=0;i<2;++i) {
        r.arrays[i].index=(uint8_t)i;r.arrays[i].length=3;
        r.arrays[i].data=malloc(3*sizeof(uint32_t));
        for (unsigned j=0;j<3;++j) r.arrays[i].data[j]=i ? 100+j : 2500;
    }
    const payload_v3_segment_t segment={.type=2,.pulses=3,.freq=1,.subsampling=1};
    ambit_trace_result_t out;
    assert(ambit_trace_decode_store(&r,0,true,1792531200000LL,1792531203000LL,NULL,
        "arrun 1,0,2,0,3,0,1,0,0,1",&segment,1,&out)==ESP_OK);
    assert(out.points==3 && out.measure_id>0);
}
int main(void)
{
    fw[0]=1;fw[1]=4;fw[8]=2;fw[13]=7;fw[28]=3;
    strcpy((char *)cal,"Stable leaf"); float coef=1.25f; memcpy(cal+108,&coef,4);memcpy(cal+136,&coef,4);
    s_initialized=true; s_cfg.uart_query=query;s_cfg.store_event=store;s_cfg.next_id=next_id;
    s_ambit_announcement_mutex=xSemaphoreCreateMutex();
    ambit_announcement_init(&s_ambit_announcements,ambit_announcement_nvs_port());
#ifndef A3_REFERENCE
    device_commands_sensors_start(inspect,NULL);
    sensor_snapshot_t boot1,boot2; assert(sensor_inventory_snapshot(&boot1));
    device_commands_sensors_start(inspect,NULL);assert(sensor_inventory_snapshot(&boot2));
    assert(boot1.boot_id!=boot2.boot_id);
#endif
    ambit_device_info_t info;
    /* Failed initial store is retried, then persists only after the actual ack. */
#ifndef A3_REFERENCE
    fail_store=true;
    assert(cmd_ambit_device_info(0,&info).status==ESP_OK); assert(failed_stores);
    fail_store=false;
    assert(cmd_ambit_device_info(0,&info).status==ESP_OK);
#else
    assert(cmd_ambit_device_info(0,&info).status==ESP_OK);
#endif
    assert(events==1); assert(ambit_announcement_ack(&s_ambit_announcements,next_event)==ESP_OK);
    assert(setters==1 && commits==1);
    next_event=100;
    assert(cmd_ambit_set_gains(0,1,2,3,4,5,6).status==ESP_OK);
    assert(cmd_ambit_set_currents(0,7,8,9).status==ESP_OK);
    for (unsigned i=0;i<10;++i) {
#ifndef A3_REFERENCE
        revalidate();
#endif
        trace();
    }
#ifndef A3_REFERENCE
    for (unsigned i=0;i<2000;++i) { fail_cal=i>=1000 && i%2; revalidate(); }
    assert(refresh_reads>=2000 && events==1 && setters==1 && commits==1);
    assert(cmd_ambit_device_info_cached(0,&info));
    sensor_snapshot_t before,after; assert(sensor_inventory_snapshot(&before));
    query_fail=true; uint8_t run_arr[8]={2};
    assert(cmd_ambit_trigger(0,run_arr,1,0,false,100).status==ESP_ERR_TIMEOUT);
    uint8_t state; assert(cmd_ambit_poll(0,&state,100).status==ESP_ERR_TIMEOUT);
    uart_sensor_response_t response; assert(cmd_ambit_fetch(0,&response,100).status==ESP_ERR_TIMEOUT);
    for (unsigned i=0;i<100;++i) sensor_inventory_refresh();
    assert(sensor_inventory_snapshot(&after));assert(after.busy==1);
    assert(!memcmp(before.ports,after.ports,sizeof before.ports));
    assert(!memcmp(before.refresh.wire_transactions,after.refresh.wire_transactions,sizeof before.refresh.wire_transactions));
    assert(after.ports[0].identity.gains_set && after.ports[0].identity.currents_set);
    fprintf(stderr,"CONTROLLER_OK stable=1000 flapping=1000 events_delta=0 setters_delta=0 commits_delta=0 owned_timeout=1 boots_distinct=1\n");
#endif
    return 0;
}

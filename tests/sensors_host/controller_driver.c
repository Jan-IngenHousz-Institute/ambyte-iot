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
static bool query_fail, fail_store, fetch_success;
static uint8_t query_fail_mask, poll_byte;
static size_t fw_bytes = 48, cal_bytes = 140;
static unsigned fw_queries, cal_queries;
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
    if (query_fail || (query_fail_mask & (1u << ch))) return ESP_ERR_TIMEOUT;
    if (cmd[0]==33) {
        size_t n = cmd[1]==2 ? fw_bytes : cal_bytes;
        if (cmd[1]==2) ++fw_queries; else ++cal_queries;
        assert(n<=expected);r->raw=malloc(expected);r->raw_len=n;
        memcpy(r->raw,cmd[1]==2 ? fw : cal,n);
        if(cmd[1]==2 && n==48) r->raw[8]+=(uint8_t)ch;
    } else if (cmd[0]==AMBIT_CMD_FETCH && fetch_success) {
        r->array_count=1;r->arrays[0].index=1;r->arrays[0].length=2;
        r->arrays[0].data=calloc(2,sizeof(uint32_t));
    } else if (expected != UART_QUERY_ACK_ONLY && expected) {
        r->raw=calloc(1,expected); r->raw_len=expected;
        if(cmd[0]==AMBIT_CMD_STATUS) r->raw[0]=poll_byte;
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
static bool recovery_proof;
static unsigned recovery_reads;
static sensor_recovery_result_t recover(uint8_t ch,uint64_t epoch,bool poll,uint32_t timeout,
    bool (*current)(uint8_t,uint64_t),void (*started)(uint8_t))
{
    (void)poll;assert(timeout==900 && current(ch,epoch));
    ++recovery_reads;started(ch);virtual_us+=200000;
    if (!recovery_proof) return SENSOR_RECOVERY_FAILED;
    virtual_us+=200000;started(ch);virtual_us+=200000;
    started(ch);virtual_us+=100000;
    return current(ch,epoch) ? SENSOR_RECOVERY_PROVEN : SENSOR_RECOVERY_FAILED;
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
#ifndef A3_REFERENCE
static bool output_json(void *ctx,const void *p,size_t n)
{ (void)ctx;return fwrite(p,1,n,stdout)==n; }
static void snapshot_json(void)
{ sensor_snapshot_t s;assert(sensor_inventory_snapshot(&s));assert(sensor_snapshot_json(&s,output_json,NULL));puts(""); }
#endif
int main(int argc, char **argv)
{
    (void)argc;(void)argv;
    fw[0]=1;fw[1]=4;fw[8]=2;fw[13]=7;fw[28]=3;
    strcpy((char *)cal,"Stable leaf"); float coef=1.25f; memcpy(cal+108,&coef,4);memcpy(cal+136,&coef,4);
    s_initialized=true; s_cfg.uart_query=query;s_cfg.store_event=store;s_cfg.next_id=next_id;
    s_ambit_announcement_mutex=xSemaphoreCreateMutex();
    ambit_announcement_init(&s_ambit_announcements,ambit_announcement_nvs_port());
#ifndef A3_REFERENCE
    device_commands_sensors_start(inspect,recover,NULL);
    sensor_snapshot_t boot1,boot2; assert(sensor_inventory_snapshot(&boot1));
    device_commands_sensors_start(inspect,recover,NULL);assert(sensor_inventory_snapshot(&boot2));
    assert(boot1.boot_id!=boot2.boot_id);
#endif
    ambit_device_info_t info;
#ifndef A3_REFERENCE
    if(argc==2 && (!strcmp(argv[1],"recovery-jobs") || !strcmp(argv[1],"recovery-stalled"))) {
        bool stalled=!strcmp(argv[1],"recovery-stalled");
        assert(cmd_ambit_device_info(0,&info).status==ESP_OK);
        assert(cmd_ambit_set_gains(0,1,2,3,4,5,6).status==ESP_OK);
        assert(cmd_ambit_set_currents(0,7,8,9).status==ESP_OK);
        sensor_snapshot_t original,snapshot;assert(sensor_inventory_snapshot(&original));
        uint8_t arr[8]={2,0,0,3,0,2}; /* 3/2s, ceil + slow setup/segment */
        const int64_t expected=17300;
        query_fail_mask=1;
        for(unsigned job=0;job<(stalled ? 1u : 2u);++job) {
            unsigned before=recovery_reads, before_fw=fw_queries;
            assert(cmd_ambit_trigger(0,arr,1,0,false,100).status==ESP_ERR_TIMEOUT);
            uint64_t epoch=sensor_inventory_run_epoch(0);
            assert(epoch>job && sensor_inventory_blocked(1));
            virtual_us+=(expected-1)*1000;
            for(unsigned i=0;i<100;++i) sensor_inventory_tick();
            assert(recovery_reads==before && sensor_inventory_busy());
            assert(cmd_ambit_device_info(1,&info).status!=ESP_OK && fw_queries==before_fw);
            assert(sensor_inventory_snapshot(&snapshot));
            assert(!snapshot.refresh.wire_transactions[0]);
            virtual_us+=1000;recovery_proof=false;sensor_inventory_tick();
            assert(recovery_reads==before+1 && sensor_inventory_busy());
            assert(cmd_ambit_device_info(1,&info).status==ESP_OK && info.valid);
            assert(sensor_inventory_snapshot(&snapshot) && !memcmp(&original.ports[0],&snapshot.ports[0],sizeof original.ports[0]));
            if(stalled) {
                virtual_us+=60000000;sensor_inventory_tick();
                virtual_us+=120000000;sensor_inventory_tick();
                assert(sensor_inventory_snapshot(&snapshot) && snapshot.stalled==1 && snapshot.busy==1);
                sensor_inventory_invalidate(1);unsigned prior_fw=fw_queries;
                assert(cmd_ambit_device_info(1,&info).status==ESP_OK && info.valid && fw_queries==prior_fw+1);
                unsigned cap=recovery_reads;virtual_us+=1000000000;
                for(unsigned i=0;i<20;++i) {sensor_inventory_tick();virtual_us+=250000;}
                assert(sensor_inventory_snapshot(&snapshot) && recovery_reads==cap && snapshot.stalled==1);
                assert(!snapshot.refresh.wire_transactions[0] && snapshot.refresh.wire_transactions[1]>0);
                assert(snapshot.recovery_wire_transactions[0]==3);
                assert(!memcmp(&original.ports[0],&snapshot.ports[0],sizeof original.ports[0]));
                snapshot_json();return 0;
            }
            /* The old leaf really measures until just before this threshold.
             * No proof traffic precedes it, and failed checks cannot expire it. */
            virtual_us+=59999000;sensor_inventory_refresh();assert(recovery_reads==before+1 && sensor_inventory_busy());
            virtual_us+=1000;recovery_proof=true;sensor_inventory_tick();
            assert(recovery_reads==before+2 && !sensor_inventory_busy());
            assert(sensor_inventory_snapshot(&snapshot) && !memcmp(&original.ports[0],&snapshot.ports[0],sizeof original.ports[0]));
            assert(snapshot.recovery_wire_transactions[0]==4*(job+1));
            /* Make ch1 uncached again without touching ch0's configuration. */
            sensor_inventory_invalidate(1);

        }
        fprintf(stderr,"RECOVERY_CONTROLLER_OK jobs=2 no_time_release=1 other_port_identity=1\n");
        snapshot_json();return 0;
    }
    if(argc==4 && !strcmp(argv[1],"legacy-short")) {
        fw_bytes=(size_t)strtoul(argv[2],NULL,10);cal_bytes=(size_t)strtoul(argv[3],NULL,10);
        cmd_result_t result=cmd_ambit_device_info(0,&info);
        assert(fw_bytes==48 ? result.status==ESP_OK : result.status!=ESP_OK);
        sensor_snapshot_t observed;assert(sensor_inventory_snapshot(&observed));
        assert(!observed.ports[0].have_cal && !observed.ports[0].have_spec);
        assert(!observed.ports[0].identity.ambit_name[0] && observed.ports[0].identity.cal_version==0);
        assert(!events && !setters && !commits && fw_queries==1);
        assert(cal_queries==(fw_bytes==48 ? AMBIT_CAL_FETCH_TRIES : 0));
        if(fw_bytes!=48) assert(!observed.ports[0].identity.valid && !observed.ports[0].generation);
        snapshot_json();return 0;
    }
    if(argc==2 && !strcmp(argv[1],"partial-trigger")) {
        for(unsigned ch=0;ch<2;++ch) {
            assert(cmd_ambit_device_info(ch,&info).status==ESP_OK);
            assert(cmd_ambit_set_gains(ch,1,2,3,4,5,6).status==ESP_OK);
            assert(cmd_ambit_set_currents(ch,7,8,9).status==ESP_OK);
        }
        sensor_snapshot_t before,after;assert(sensor_inventory_snapshot(&before));
        uint8_t arr[8]={2};query_fail_mask=1;
        assert(cmd_ambit_trigger(0,arr,1,0,false,100).status==ESP_ERR_TIMEOUT);
        assert(cmd_ambit_trigger(1,arr,1,0,false,100).status==ESP_OK);
        fetch_success=true;uart_sensor_response_t response={0};
        assert(cmd_ambit_fetch(1,&response,100).status==ESP_OK);uart_sensor_response_free(&response);
        for(unsigned i=0;i<100;++i) sensor_inventory_refresh();
        assert(sensor_inventory_snapshot(&after) && after.busy==1);
        assert(!memcmp(before.ports,after.ports,sizeof before.ports));
        assert(sensor_inventory_wait_ms()==15800);
        assert(!memcmp(before.refresh.wire_transactions,after.refresh.wire_transactions,sizeof before.refresh.wire_transactions));
        snapshot_json();return 0;
    }
#endif
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
    /* A transport-successful garbage status is still unproven. It must not
     * invalidate measurement provenance or clear the acquisition overlay. */
    query_fail=false;
    const uint8_t bad_states[]={3,255};
    for(unsigned i=0;i<sizeof bad_states;++i) {
        poll_byte=bad_states[i];
        assert(cmd_ambit_poll(0,&state,100).status==ESP_OK && state==poll_byte);
        for(unsigned j=0;j<100;++j) sensor_inventory_refresh();
        assert(sensor_inventory_snapshot(&after) && after.busy==1);
        assert(!memcmp(before.ports,after.ports,sizeof before.ports));
        assert(!memcmp(before.refresh.wire_transactions,after.refresh.wire_transactions,sizeof before.refresh.wire_transactions));
    }
    poll_byte=AMBIT_ASYNC_IDLE;
    /* Conservative busy excludes refresh, but cannot deadlock an explicit
     * acquisition retry on that same port after a lost trigger ACK. */
    query_fail=false;
    assert(cmd_ambit_trigger(0,run_arr,1,0,false,100).status==ESP_OK);
    assert(cmd_ambit_poll(0,&state,100).status==ESP_OK && state==AMBIT_ASYNC_IDLE);
    assert(!sensor_inventory_busy());
    unsigned reads_before=refresh_reads; revalidate();assert(refresh_reads>reads_before);
    fprintf(stderr,"CONTROLLER_OK stable=1000 flapping=1000 events_delta=0 setters_delta=0 commits_delta=0 owned_timeout=1 boots_distinct=1\n");
#endif
    return 0;
}

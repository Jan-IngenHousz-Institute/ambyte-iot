/* Production OTA worker, Jobs adapter and admission; SDK/network boundaries only. */
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../components/ota_update/ota_update.c"
#include "iot_jobs.h"
#include "esp_image_format.h"

static char trace[1024], last_report[384];
static size_t used;
static const char *scenario;
static jmp_buf reset;
static unsigned selections, latches, restarts, successes, valid_marks, rejected;
static int64_t now;
static bool health;
static uint8_t run_bytes[8192], next_bytes[8192];
static const esp_partition_t run = {.address=0x10000,.size=8192};
static const esp_partition_t next = {.address=0x310000,.size=8192};
static void (*work)(void *);
static void *work_arg;
static void event(char c) { assert(used+1<sizeof trace);trace[used++]=c;trace[used]=0; }

void svc_test_enter(void) {}
void svc_test_exit(void) {}
const char *esp_err_to_name(esp_err_t e) { (void)e;return "stub-error"; }
void vTaskDelay(TickType_t ticks) { now+=(int64_t)ticks*1000; }
int64_t esp_timer_get_time(void) {return now;}
void esp_restart(void) { event('X');++restarts;longjmp(reset,1); }
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t d={.version="2.8.0-control"};return &d; }
esp_err_t nvs_open(const char *ns,nvs_open_mode_t mode,nvs_handle_t *h)
{ assert(!strcmp(ns,"ota_upd"));*h=1;return mode==NVS_READONLY?ESP_ERR_NVS_NOT_FOUND:ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *o,size_t *n)
{(void)h;(void)k;(void)o;(void)n;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *v)
{(void)h;assert(!strcmp(k,"applied_id")&&*v);++latches;event('L');return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
const esp_partition_t *esp_ota_get_running_partition(void){return &run;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p){(void)p;return &next;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *state)
{assert(p==&run);*state=health?ESP_OTA_IMG_PENDING_VERIFY:ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){++valid_marks;return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){event('K');return ESP_ERR_OTA_ROLLBACK_FAILED;}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t off,void *out,size_t n)
{
    assert(off+n<=8192);
    if(n==320)event(p==&run?'R':'M');
    memcpy(out,(p==&run?run_bytes:next_bytes)+off,n);return ESP_OK;
}
esp_err_t esp_image_verify(int mode,const esp_partition_pos_t *p,esp_image_metadata_t *m)
{
    (void)mode;assert(p->offset==next.address);event('S');
    if(!strcmp(scenario,"stale_tail")) {
        /* A genuinely signed 8KiB fixture remains in the slot. Only its first
         * 4KiB arrived this time. Model the pinned SDK's signature-sector bound:
         * using the whole slot would validate stale signature bytes. */
        assert(next_bytes[4096]==0xe7&&next_bytes[4097]==2);
        m->image_len=8192;
        if(p->size<8192){++rejected;return ESP_FAIL;}
        return ESP_OK;
    }
    assert(p->size==8192);
    m->image_len=8192;
    if(!strcmp(scenario,"signature")){++rejected;return ESP_FAIL;}
    return ESP_OK;
}
esp_err_t esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *cfg,esp_https_ota_handle_t *out)
{assert(strncmp(cfg->http_config->url,"https://",8)==0);*out=(void*)1;return ESP_OK;}
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h)
{assert(h);event('P');return !strcmp(scenario,"legacy_error")?ESP_ERR_INVALID_VERSION:ESP_OK;}
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h)
{assert(h);event('C');return strcmp(scenario,"incomplete")!=0;}
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t h)
{assert(h);return !strcmp(scenario,"stale_tail")?4096:8192;}
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t h,esp_app_desc_t *d)
{assert(h);event('V');snprintf(d->version,sizeof d->version,"%s",!strcmp(scenario,"bad_version")?"wrong":"2.8.0-next");return ESP_OK;}
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h){assert(h);event('F');++selections;return ESP_OK;}
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h){assert(h);event('A');return ESP_OK;}
void mbedtls_sha256_init(mbedtls_sha256_context *c){(void)c;}
int mbedtls_sha256_starts(mbedtls_sha256_context *c,int t){(void)c;(void)t;event('H');return 0;}
int mbedtls_sha256_update(mbedtls_sha256_context *c,const unsigned char *p,size_t n){(void)c;(void)p;(void)n;return 0;}
int mbedtls_sha256_finish(mbedtls_sha256_context *c,unsigned char *o){(void)c;memset(o,!strcmp(scenario,"bad_hash")?0:0xaa,32);return 0;}
void mbedtls_sha256_free(mbedtls_sha256_context *c){(void)c;}
esp_err_t fleet_jitter_slot_for_sta_mac(uint32_t slots,uint32_t *out){assert(slots==900);*out=0;return ESP_OK;}
static bool connected(void){return !health;}
static void workload_resume(void){event('W');}
static void comms_resume(void){event('N');}
bool ota_refusal_save(const char *id,const char *source,const char *reason)
{assert(*id&&*source&&*reason);event('D');return strcmp(scenario,"persist_fail")!=0;}
static esp_err_t publish(const char *topic,const char *p,size_t n,int *id)
{
    (void)id;assert(n<sizeof last_report);
    if(!strcmp(topic,"device/status")) {
        memcpy(last_report,p,n);last_report[n]=0;
        if(strstr(last_report,"\"state\":\"failed\""))event('E');
        if(strstr(last_report,"\"state\":\"success\""))++successes;
    } else if(strstr(topic,"/update")) {
        if (strstr(p,"\"status\":\"FAILED\"")) event('B');
        else assert(strstr(p,"\"status\":\"IN_PROGRESS\""));
    }
    return ESP_OK;
}
static bool submit(void (*callback)(void *),void *arg){assert(!work);work=callback;work_arg=arg;return true;}
typedef struct{message_received_fn message;message_suback_fn suback;void *ctx;} subscription;
static subscription subs[2];static unsigned sub_count;
static esp_err_t subscribe(const char *topic,message_received_fn message,message_suback_fn ack,void *ctx)
{(void)topic;assert(sub_count<2);subs[sub_count++]=(subscription){message,ack,ctx};return ESP_OK;}
static void connection_stats(uint32_t *n,int64_t *age,char *r,size_t c){(void)r;(void)c;*n=1;*age=0;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *h){(void)a;*h=(void*)1;return ESP_OK;}
esp_err_t esp_timer_stop(esp_timer_handle_t h){(void)h;return ESP_OK;}
esp_err_t esp_timer_start_once(esp_timer_handle_t h,uint64_t d){(void)h;(void)d;return ESP_OK;}

static void image(uint8_t *p)
{
    p[0]=0xe9;p[1]=1;p[12]=9;p[28]=0x20;p[29]=1;
    p[32]=0x32;p[33]=0x54;p[34]=0xcd;p[35]=0xab;
    memcpy(p+OTA_MANIFEST_OFFSET,ota_storage_manifest,32);
}

int main(int argc,char **argv)
{
    assert(argc>=3&&argc<=5);scenario=argv[2];unsigned cycles=argc>=4?(unsigned)atoi(argv[3]):1;assert(cycles>0&&cycles<=20);health=!strcmp(argv[1],"health");
    image(run_bytes);image(next_bytes);
    if(!strcmp(scenario,"stale_tail")) {
        assert(argc==5);FILE *f=fopen(argv[4],"rb");assert(f);
        assert(fread(next_bytes,1,sizeof next_bytes,f)==sizeof next_bytes);
        assert(fgetc(f)==EOF);assert(fclose(f)==0);
    }
    if(!strcmp(scenario,"incompatible")){next_bytes[0x120+16]=6;next_bytes[0x120+20]=2;}
    if(!strcmp(scenario,"running_bad"))run_bytes[0x120]=0;
    if(!strcmp(scenario,"candidate_bad"))next_bytes[0x120]=0;
    if(!strcmp(scenario,"persist_fail")){next_bytes[0x120+16]=6;next_bytes[0x120+20]=2;}
    ota_update_config_t cfg={.publish=publish,.is_connected=connected,.status_topic="device/status",
        .device_id="test",.submit=submit,.workload_resume=workload_resume,.comms_resume=comms_resume};
    assert(ota_update_init(&cfg)==ESP_OK);
    if(!setjmp(reset)) {
        if(health)ota_update_run_boot_confirm();
        else for(unsigned cycle=0;cycle<cycles;++cycle) {
            if(!strcmp(argv[1],"jobs")) {
                iot_jobs_config_t jobs={.thing_name="AMBYTE_TEST",.family="ambyte",.running_version="2.8.0-control",
                    .publish=publish,.add_subscription=subscribe,.connection_stats=connection_stats};
                if(cycle==0) {
                    assert(iot_jobs_init(&jobs)==ESP_OK);
                    subs[0].suback(true,1,subs[0].ctx);subs[1].suback(true,1,subs[1].ctx);
                }
                const char *doc="{\"execution\":{\"jobId\":\"a5-next\",\"status\":\"QUEUED\",\"jobDocument\":{\"operation\":\"firmware-update\",\"family\":\"ambyte\",\"version\":\"2.8.0-next\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"url\":\"https://example.test/app.bin\"}}}";
                subs[0].message("jobs/notify",doc,strlen(doc),subs[0].ctx);
            } else assert(ota_update_request("https://example.test/app.bin","a5-next")==ESP_OK);
            assert(work);work(work_arg);work=NULL;work_arg=NULL;
        }
    } else if(work_arg)free(work_arg); /* simulated reset frees reset-owned request */
    if(health) {
        assert(restarts==1&&selections==0&&valid_marks==0&&successes==0&&latches==0);
        assert(!strcmp(trace,"KX"));
    } else if(!strcmp(scenario,"accept")) {
        assert(selections==1&&latches==1&&restarts==1);
        assert(strstr(trace,"SRMFLX"));
    } else {
        assert(selections==0&&latches==0&&restarts==0&&successes==0);
        assert(strstr(trace,"AWNDE"));
        if(!strcmp(argv[1],"jobs"))assert(strstr(trace,"AWNDEB"));
        if(!strcmp(scenario,"signature"))assert(rejected==1&&!strchr(trace,'R')&&!strchr(trace,'M'));
        if(!strcmp(scenario,"stale_tail")) {
            assert(rejected==1&&!strchr(trace,'R')&&!strchr(trace,'M'));
            assert(strstr(last_report,"signature_invalid"));
        }
        if(!strcmp(scenario,"bad_version"))assert(!strchr(trace,'P')&&!strchr(trace,'H')&&!strchr(trace,'S'));
        if(!strcmp(scenario,"bad_version"))assert(strstr(last_report,"image is wrong, job expects 2.8.0-next"));
        if(!strcmp(scenario,"bad_hash"))assert(strstr(last_report,"sha256 mismatch: downloaded image is not the released one"));
        if(!strcmp(scenario,"legacy_error"))assert(strstr(last_report,"not_an_image: URL served non-firmware (HTML?)"));
        if(!strcmp(scenario,"persist_fail"))assert(strstr(last_report,"refusal_persist_failed: fallback_cannot_read_written"));
    }
    printf("TRACE=%s\n",trace);
    return 0;
}

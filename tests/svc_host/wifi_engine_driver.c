/* Production Wi-Fi manager policy/events + durable production service engine.
 * Only the IDF driver/event ports are simulated, including the documented
 * associated set_config reason=1 drop, both inline and queued. */
#define main manager_policy_main
#include "../wifi_manager_host.c"
#undef main
#include "svc_engine.h"
#include <assert.h>
#include <openssl/evp.h>

static bool engine_held, maint_held;
static void (*service_worker)(void *);
static void *service_arg;
static void engine_lock(void *c) { (void)c;assert(!engine_held);engine_held=true; }
static void engine_unlock(void *c) { (void)c;assert(engine_held);engine_held=false; }
static bool maint_begin(void *c) { (void)c;assert(!engine_held && !maint_held);maint_held=true;return true; }
static void maint_end(void *c) { (void)c;assert(!engine_held && maint_held);maint_held=false; }
static bool service_ready(void *c) { (void)c;return true; }
static int64_t service_now(void *c) { (void)c;return g_now_us/1000; }
static bool digest(void *c,const void *p,size_t n,unsigned char out[32])
{ (void)c;unsigned length=0;return EVP_Digest(p,n,out,&length,EVP_sha256(),NULL)==1 && length==32; }
static bool submit_service(void *c,void (*run)(void *),void *arg)
{ (void)c;assert(engine_held && !service_worker);service_worker=run;service_arg=arg;return true; }
static size_t free_service(void *c) { (void)c;return 1024*1024; }
void svc_store_fault(const char *point) { (void)point; }
static void real_trial(void *c,const char *ssid,const char *password,svc_prepare_fn prepare,void *ctx,svc_trial_result_t *out)
{
    (void)c;assert(!engine_held && maint_held);
    wifi_manager_trial_result_t result;
    esp_err_t error=wifi_manager_trial(ssid,password,prepare,ctx,&result);
    out->reason=error==ESP_OK?NULL:"wifi_timeout";
    out->saved=result.saved;out->connected=result.connected;out->retrying=result.retrying;
    out->restored=result.restored;out->persist_intent=result.persist_intent;
    trial_wipe(&result,sizeof result);
}
int main(int argc,char **argv)
{
    assert(argc==3);boot("OldAP");
    bool inline_drop=!strcmp(argv[2],"inline"), stale=!strcmp(argv[2],"stale");
    g_set_config_delivers_now=inline_drop;g_drop_connect_in_set_config=true;
    g_script[0]=SCRIPT_GOT_IP;g_script[1]=stale?SCRIPT_GOT_IP_ONLY:SCRIPT_GOT_IP;g_script_len=2;
    svc_store_t store;assert(!svc_store_open(&store,argv[1],1,free_service,NULL));
    svc_engine_t engine;svc_engine_ports_t ports={.protocol={.sha256=digest},.lock=engine_lock,.unlock=engine_unlock,
        .submit=submit_service,.maintenance_begin=maint_begin,.maintenance_end=maint_end,.boot_ready=service_ready,
        .now_ms=service_now,.wifi_trial=real_trial};
    svc_engine_init(&engine,&store,&ports);
    char bind[]="svc.bind 0000000000000001",json[SVC_JSON_MAX+1];svc_engine_dispatch(&engine,bind,json);
    char join[]="svc.wifi.join 0000000000000001 VHJpYWxBUA== dHJpYWwtcGFzcw==";
    svc_engine_dispatch(&engine,join,json);assert(service_worker && strstr(json,"connecting"));
    service_worker(service_arg);assert(!engine_held && !maint_held && !engine.worker_owned);
    svc_record_t result;assert(!svc_store_get(&store,1,&result));
    bool current=wifi_manager_is_connected() && wifi_manager_link_is_current() && g_associated;
    if(!stale) {
        assert(result.state==SVC_SUCCEEDED && result.saved==SVC_TRUE && result.connected==SVC_TRUE);
        assert(current && !result.retrying && !g_timer_armed && !s_wifi.reconfigure_in_progress);
    } else assert(result.state==SVC_UNCERTAIN && result.saved==SVC_UNKNOWN && result.connected==SVC_FALSE && !current);
    assert(g_flash_writes==1 && g_connect_calls==2 && !s_service_trial && g_now_us<30000000);
    assert(log_count('I',"disconnected for reconfigure (reason=1)")==1);
    assert(log_count('W',"Wi-Fi disconnected (reason=1)")==0);
    svc_protocol_result(&engine.protocol,&result,json);puts(json);
    return 0;
}

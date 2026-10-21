/* Hardware-free execution of the exact resident-worker and AMBIT op functions. */
#include "sensor_inventory.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
typedef unsigned BaseType_t;
typedef void *QueueHandle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#ifndef HOST_TICK_MS
#define HOST_TICK_MS 1
#endif
#define pdMS_TO_TICKS(t) ((t)/HOST_TICK_MS)
#define pdTRUE 1
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
#define ESP_LOGW(...) ((void)0)
#define TAG "worker_host"
static portMUX_TYPE s_maint_mux;
static bool s_maint_busy;
static QueueHandle_t s_maint_q=(void *)1;
typedef void *TaskHandle_t;
static TaskHandle_t s_maint_task;
static unsigned notifications;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static BaseType_t xTaskNotifyGive(TaskHandle_t t) { assert(t);++notifications;return pdTRUE; }
static uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks);
static int64_t clock_us;
static bool boot_ready=true;
static int target_type, target_op, jobs_run, reads, reads_at_queue;
static int64_t queued_at;
static jmp_buf done;
static bool fairness_mode, fairness_absent;
static unsigned positive_waits, consecutive_ready;
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool app_service_boot_ready(void) { return boot_ready; }
static void ota_update_run_boot_confirm(void) {}
static BaseType_t xQueueSend(QueueHandle_t,const void *,TickType_t);
static BaseType_t xQueueReceive(QueueHandle_t,void *,TickType_t);
static unsigned uxQueueMessagesWaiting(QueueHandle_t);
static void guarded_op(void);
static void sched_runner_cleanup_lights(void)
{
    assert(sensor_inventory_wait_ms()==UINT32_MAX);
    unsigned before=(unsigned)reads; sensor_inventory_refresh();assert(before==(unsigned)reads);
}
typedef struct { unsigned op,channel; char id[8]; } ambit_ota_req_t;
#define AMBIT_OP_VERSIONS 1
#define AMBIT_OP_PROBE 2
#define AMBIT_OP_FLASH 3
static struct { bool (*maintenance_begin)(void); void (*maintenance_end)(void); } ambit_cfg;
static void ambit_do_versions(const char *id) { (void)id; guarded_op(); }
static void ambit_do_probe(const ambit_ota_req_t *r) { (void)r; guarded_op(); }
static void ambit_do_flash(const ambit_ota_req_t *r) { (void)r; guarded_op(); }
static void ambit_do_ota(const ambit_ota_req_t *r) { (void)r; guarded_op(); }
static void report_busy(const ambit_ota_req_t *r) { (void)r; assert(0); }
#include "worker-production.inc"
static maint_job_t queue[4];
static unsigned qcount;
static BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t t)
{ (void)q;(void)t; if(qcount==4) return 0;queue[qcount++]=*(const maint_job_t *)item;return pdTRUE; }
static unsigned uxQueueMessagesWaiting(QueueHandle_t q) { (void)q;return qcount; }
static BaseType_t xQueueReceive(QueueHandle_t q,void *out,TickType_t ticks)
{
    (void)q;assert(ticks==0);
    if(qcount) { *(maint_job_t *)out=queue[0];memmove(queue,queue+1,--qcount*sizeof *queue);return pdTRUE; }
    if(jobs_run==2) {
        assert(sensor_inventory_acquire(3));
        /* The following worker iteration must really block indefinitely. */
        jobs_run++;
        return 0;
    }
    return 0;
}
static uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ticks)
{
    assert(clear);
    if(fairness_mode) {
        assert(++consecutive_ready<64); /* A ready notification is not idle time. */
        if(notifications) { unsigned n=notifications;notifications=0;return n; }
        assert(ticks!=portMAX_DELAY);
        if(!ticks) return 0; /* One due block is immediate; the next must wait. */
        ++positive_waits;consecutive_ready=0;
        clock_us+=(int64_t)ticks*HOST_TICK_MS*1000;
        if(clock_us>=600000000) longjmp(done,1);
        return 0;
    }
    if (notifications) { unsigned n=notifications;notifications=0;return n; }
    if(jobs_run==3 || !boot_ready) { assert(ticks==portMAX_DELAY);longjmp(done,1); }
    assert(ticks!=portMAX_DELAY && ticks<=60000);
    clock_us+=(int64_t)ticks*HOST_TICK_MS*1000;
    return 0;
}
static void no_lock(void) {}
static void yield_ms(uint32_t ms) { clock_us+=(int64_t)ms*1000; }
static void guarded_op(void)
{
    assert(s_maint_busy && sensor_inventory_wait_ms()==UINT32_MAX);
    int before=reads; sensor_inventory_refresh();assert(before==reads);
    ++jobs_run;
}
static void service_job(void *arg)
{
    (void)arg; assert(clock_us-queued_at<=1000000);assert(reads==reads_at_queue+1);
    assert(app_maintenance_begin());assert(!app_maintenance_begin());
    guarded_op();app_maintenance_end();
}
static sensor_read_result_t wire(uint8_t ch,uint8_t type,void *out,size_t n,size_t *got,uint32_t timeout,void (*started)(uint8_t))
{
    assert(!sensor_inventory_busy());started(ch);++reads;*got=0;
    if(fairness_mode && fairness_absent) { clock_us+=(int64_t)timeout*1000;return type ? SENSOR_READ_FAILED : SENSOR_READ_ABSENT; }
    if(type==target_type && jobs_run==0) {
        queued_at=clock_us; reads_at_queue=reads-1;
        ambit_ota_req_t *req=calloc(1,sizeof *req);req->op=(unsigned)target_op;
        if (target_op==3) { assert(app_maint_submit(service_job,NULL));assert(app_maint_submit(ambit_ota_run,req)); }
        else { assert(app_maint_submit(ambit_ota_run,req));assert(app_maint_submit(service_job,NULL)); }
        assert(queue[qcount-2].behind_refresh);
    }
    clock_us+=(int64_t)timeout*1000;
    if(type) { memset(out,0,n);unsigned char *p=out;p[0]=1;
        if(type==2) {p[1]=4;p[8]=1;} *got=n; }
    return SENSOR_READ_OK;
}
int main(void)
{
    ambit_cfg.maintenance_begin=app_maintenance_begin;ambit_cfg.maintenance_end=app_maintenance_end;
    const int types[]={0,2,1,4};
    for(unsigned phase=0;phase<4;++phase) for(target_op=0;target_op<4;++target_op) {
        target_type=types[phase];jobs_run=reads=qcount=notifications=0;clock_us=0;
        sensor_inventory_ports_t ports={.now_us=esp_timer_get_time,.lock=no_lock,.unlock=no_lock,
            .yield_ms=yield_ms,.wake=app_sensors_wake,.inspect=wire};
        sensor_inventory_init(&ports,123);
        if(!setjmp(done)) app_maint_worker(NULL);
        sensor_snapshot_t snapshot;assert(sensor_inventory_snapshot(&snapshot));
        assert(jobs_run==3 && snapshot.refresh.job_start_waits==1);
        assert(snapshot.refresh.job_start_wait_max_us==900000);
        sensor_inventory_complete(3);
    }
    sched_runner_exit_cleanup();
    assert(sensor_inventory_wait_ms()!=UINT32_MAX);
    /* A notification storm cannot consume even one real job slot. */
    jobs_run=qcount=0;
    for(unsigned i=0;i<1000;++i) app_sensors_wake();
    for(unsigned i=0;i<4;++i) assert(app_maint_submit(service_job,NULL));
    assert(!app_maint_submit(service_job,NULL));
    jobs_run=qcount=notifications=0;boot_ready=false;
    if(!setjmp(done)) app_maint_worker(NULL);
    /* Run the actual notification/due-time loop for ten virtual minutes,
     * present and absent, with both1ms and production10ms scheduler ticks.
     * Every refresh cycle must reach a positive blocking wait; repeated ready
     * notifications or sub-tick due times may never become a busy spin. */
    boot_ready=true;fairness_mode=true;target_type=-1;
    for(unsigned absent=0;absent<2;++absent) {
        fairness_absent=absent!=0;positive_waits=consecutive_ready=0;
        clock_us=0;jobs_run=reads=qcount=notifications=0;
        sensor_inventory_ports_t ports={.now_us=esp_timer_get_time,.lock=no_lock,.unlock=no_lock,
            .yield_ms=yield_ms,.wake=app_sensors_wake,.inspect=wire};
        sensor_inventory_init(&ports,123);
        if(!setjmp(done)) app_maint_worker(NULL);
        assert(reads>10 && positive_waits>10);
    }
    puts("WORKER_OK phases=4 ops=4 priority=1 computed_wait=1 indefinite=1 job_wait_us=900000");
    puts("WORKER_FAIRNESS_OK minutes=10 present=1 absent=1");
}

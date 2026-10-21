#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../components/sched_runner/sched_runner.c"
static int depth;
static int64_t test_us;
static unsigned actions, inspection;
static bool fail_action;
const char *esp_err_to_name(esp_err_t e) { (void)e;return "host_error"; }
void identity_host_enter(portMUX_TYPE *m) { (void)m; ++depth; }
void identity_host_exit(portMUX_TYPE *m) { (void)m; assert(depth-->0); }
int64_t esp_timer_get_time(void) { return test_us; }
static void lock_cache(void) { assert(depth==0); }
static void unlock_cache(void) { assert(depth==0); }
static void yield_ms(uint32_t ms) { test_us+=(int64_t)ms*1000; }
static sensor_read_result_t inspect(uint8_t ch,uint8_t type,void *out,size_t n,size_t *got,uint32_t timeout,void (*started)(uint8_t))
{ (void)ch;(void)type;(void)out;(void)n;(void)got;(void)timeout;(void)started;++inspection;return SENSOR_READ_FAILED; }
static esp_err_t action(void *ctx,const sched_step_t *step,const sched_program_t *prog)
{
    (void)ctx;(void)step;(void)prog;
    assert(sensor_inventory_wait_ms()==UINT32_MAX && depth==0);
    for(unsigned i=0;i<100;++i) { yield_ms(100);sensor_inventory_refresh(); }
    assert(inspection==0);++actions;
    return fail_action ? ESP_FAIL : ESP_OK;
}
int main(void)
{
    const sensor_inventory_ports_t ports={.now_us=esp_timer_get_time,.lock=lock_cache,
        .unlock=unlock_cache,.yield_ms=yield_ms,.inspect=inspect};
    sensor_inventory_init(&ports,123);
    strcpy(s_prog.pool,"spectra");s_prog.pool_used=8;s_prog.job_count=1;
    const sched_action_t declaration={.name="ambit/spectrum",.run=action};
    s_prog.jobs[0].step_count=2;s_prog.jobs[0].steps[0].action=&declaration;
    s_prog.jobs[0].steps[1].action=&declaration;
    run_job(0);assert(actions==2 && !depth && sensor_inventory_wait_ms()!=UINT32_MAX);
    fail_action=true;run_job(0);assert(actions==3 && !depth && sensor_inventory_wait_ms()!=UINT32_MAX);
    s_should_stop=true;run_job(0);assert(actions==3 && sensor_inventory_wait_ms()!=UINT32_MAX);
    puts("SENSOR_RUNNER_OK complete_job_veto=1 failure_release=1 stop_release=1");
}

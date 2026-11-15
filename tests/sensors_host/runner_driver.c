#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../components/sched_runner/sched_runner.c"
/* Brownout protection seam (power_protect.h): the state run_job sheds by. */
static pp_state_t pp_now = PP_STATE_NORMAL;
static unsigned pp_flash_skips, pp_ambit_skips, cleanups;
pp_state_t power_protect_state(void) { return pp_now; }
void power_protect_note_skip(power_protect_skip_t w)
{ if (w == POWER_PROTECT_SKIP_FLASH_RUN) ++pp_flash_skips; else if (w == POWER_PROTECT_SKIP_AMBIT_RUN) ++pp_ambit_skips; else assert(0); }
const char *pp_state_name(pp_state_t s) { return s == PP_STATE_HOLD ? "hold" : s == PP_STATE_NO_FLASH ? "no_flash" : "normal"; }
void sched_runner_cleanup_lights(void) { ++cleanups; }
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
    /* Brownout protection: no_flash keeps a spectra job; hold sheds it before
     * any action or the inspection bracket, as a skip, never a failure. */
    s_should_stop=false;fail_action=false;pp_now=PP_STATE_NO_FLASH;
    run_job(0);assert(actions==5 && pp_ambit_skips==0 && cleanups==0);
    const sched_job_counts_t before=s_rt[0].n;
    pp_now=PP_STATE_HOLD;s_deferred=1u;
    run_job(0);
    assert(actions==5 && pp_ambit_skips==1 && pp_flash_skips==0 && cleanups==1);
    assert(s_rt[0].n.runs==before.runs && s_rt[0].n.failures==before.failures &&
           s_rt[0].n.fail_streak==before.fail_streak && s_rt[0].n.power_skipped==before.power_skipped+1);
    assert(s_deferred==0 && s_running_job==-1 && !depth && sensor_inventory_wait_ms()!=UINT32_MAX);
    pp_now=PP_STATE_NORMAL;run_job(0);assert(actions==7 && s_rt[0].n.shed_streak==0);
    puts("SENSOR_RUNNER_OK complete_job_veto=1 failure_release=1 stop_release=1 power_shed=1");
}

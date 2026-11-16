#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/semphr.h"
#ifndef ESP_ERR_TIMEOUT
#define ESP_ERR_TIMEOUT 0x107
#endif
/* Compile the production adapter unchanged; same TU exposes only test setup
 * (the SDK calls below are the fake hardware). */
#include "../components/mp2731/mp2731.c"

static int64_t clock_us;
static int mutex_held;
static int fail_reg = -1;
static bool timeout_next, nested_timeout;
static uint8_t regs[24] = {[0]=0x48,[1]=6,[2]=0xDC,[3]=0x10,[0x0C]=0x30,[0x0D]=0x80,
                          [0x0E]=195,[0x0F]=200,[0x10]=102,[0x11]=85,[0x12]=10,[0x13]=30};
void h_critical_enter(portMUX_TYPE *m) { assert(!m->held); m->held=1; }
void h_critical_exit(portMUX_TYPE *m) { assert(m->held); m->held=0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_held; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    assert(m == &mutex_held);
    if (timeout_next || mutex_held) { timeout_next=false; clock_us+=(int64_t)wait*1000; return pdFALSE; }
    mutex_held=1; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { assert(m==&mutex_held && mutex_held); mutex_held=0; return pdTRUE; }
int64_t esp_timer_get_time(void) { return clock_us; }
void esp_rom_delay_us(unsigned us) { clock_us+=us; }
void vTaskDelay(TickType_t ticks) { clock_us+=(int64_t)ticks*1000; }
esp_err_t i2c_bus_get_port(i2c_port_t *p) { *p=0; return ESP_OK; }
esp_err_t i2c_bus_lock(unsigned timeout) { (void)timeout; return ESP_OK; }
esp_err_t i2c_bus_unlock(void) { return ESP_OK; }
esp_err_t i2c_bus_probe_addr(i2c_port_t p,unsigned a) { (void)p;(void)a;return ESP_OK; }
esp_err_t i2c_master_write_read_device(int port,unsigned addr,const uint8_t *w,size_t wn,uint8_t *r,size_t rn,unsigned timeout)
{
    (void)port;(void)addr;(void)timeout; assert(wn==1 && rn==1);
    if (*w==0x0E && nested_timeout) {
        nested_timeout=false;
        power_reading_t rejected;
        assert(mp2731_read_power(&rejected)==ESP_ERR_TIMEOUT); /* A owns read mutex, B times out */
    }
    if (*w == fail_reg) return ESP_FAIL;
    *r=regs[*w]; return ESP_OK;
}
esp_err_t i2c_master_write_to_device(int port,unsigned addr,const uint8_t *w,size_t wn,unsigned timeout)
{
    (void)port;(void)addr;(void)timeout; assert(wn==2 && w[0]==3); /* never changes charge settings */
    regs[3]=w[1]&0x7f; return ESP_OK;
}
static void reset_history(void)
{
    memset(&s_history,0,sizeof s_history); s_overlap_epoch=0;s_power_reads_active=0;
    clock_us=1000000;fail_reg=-1;regs[0x0C]=0x30;regs[0x0D]=0x80;regs[0x12]=10;
}
int main(void)
{
    assert(mp2731_init()==ESP_OK);
    power_reading_t p;
    char json[CHARGER_DIAGNOSTICS_JSON_CAP];uint64_t token;
    reset_history();
    assert(mp2731_read_power(&p)==ESP_OK);
    clock_us+=5000000; timeout_next=true;
    assert(mp2731_read_power(&p)==ESP_ERR_TIMEOUT);
    clock_us+=1000000; regs[0x0C]=0;
    assert(mp2731_read_power(&p)==POWER_ERR_IMPLAUSIBLE); /* live idle with a stale current */
    assert(s_history.totals.reads==3 && s_history.totals.read_errors==2);
    assert(s_history.totals.charging_ms==0 && s_history.totals.charge_stops==0);
    uint64_t attempts=s_history.totals.reads;
    timeout_next=true;
    assert(mp2731_render_diagnostics(json,sizeof json,&token,true)<0);
    assert(s_history.totals.reads==attempts); /* diagnostic lock refusal is not a power attempt */

    reset_history();assert(mp2731_read_power(&p)==ESP_OK);
    clock_us+=5000000; regs[0x0C]=0; nested_timeout=true;
    assert(mp2731_read_power(&p)==POWER_ERR_IMPLAUSIBLE);
    assert(s_history.totals.reads==3 && s_history.totals.read_errors==2);
    assert(s_history.totals.charging_ms==0 && s_history.totals.charge_stops==0);
    /* Simulate a failure caller descheduled after its failed take: the active
     * attempt was registered before waiting, so A cannot bridge it. */
    reset_history();assert(mp2731_read_power(&p)==ESP_OK);
    uint64_t a=history_begin(),b=history_begin();
    clock_us+=5000000;mp2731_frame_t f={.status_valid=true,.status=0,.fault=0x86};
    history_finish(a,POWER_ERR_IMPLAUSIBLE,&p,&f);
    assert(!s_history.totals.charging_ms && !s_history.totals.charge_stops);
    clock_us+=1000000;history_finish(b,ESP_ERR_TIMEOUT,NULL,NULL);
    assert(s_history.totals.reads==3 && s_history.totals.last_read_ms==clock_us/1000);

    /* Every read boundary: partial status is unavailable; both live bytes
     * survive every later ADC-register failure without continuity inference. */
    const int failures[]={3,0x0C,0x0D,0x0E,0x0F,0x11,0x12,0x13};
    for (unsigned i=0;i<sizeof failures/sizeof failures[0];i++) {
        reset_history();assert(mp2731_read_power(&p)==ESP_OK);
        clock_us+=5000000;fail_reg=failures[i];regs[0x0C]=0x22;regs[0x0D]=0x16;
        assert(mp2731_read_power(&p)==ESP_FAIL);
        const bool live=fail_reg>=0x0E;
        assert(s_history.totals.status_reads==(live?2u:1u));
        assert((s_history.totals.fault_or&0x16u)==(live?0x16u:0u));
        assert(!s_history.totals.charging_ms && !s_history.totals.charge_stops);
        fail_reg=-1;regs[0x12]=0;clock_us+=1000000;
        assert(mp2731_read_power(&p)==ESP_OK);
        assert(!s_history.totals.charging_ms && !s_history.totals.charge_stops);
    }
    reset_history();assert(mp2731_read_power(&p)==ESP_OK);
    assert(mp2731_render_diagnostics(json,sizeof json,&token,true)>0);
    const uint64_t sealed=token;
    const mp2731_history_t before=s_history;
    assert(mp2731_render_diagnostics(json,sizeof json,&token,false)>0);
    assert(memcmp(&before,&s_history,sizeof before)==0); /* peek has no range/token mutation */
    mp2731_ack_diagnostics(sealed); assert(!s_history.pending_range_valid);
    puts("DRIVER_HISTORY_OK timeouts partial_frames preview_peek");
    return 0;
}

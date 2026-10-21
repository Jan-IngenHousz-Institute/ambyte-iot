/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "uart_sensors.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

const char *esp_err_to_name(esp_err_t error) { (void)error; return "test_error"; }
static _Atomic int64_t clock_us;
static pthread_mutex_t inventory_mutex = PTHREAD_MUTEX_INITIALIZER;
static int mutexes[5], mutex_count;
static unsigned char rx[512], payload[140];
static size_t rx_at, rx_size;
static unsigned writes, refresh_starts, mode;
static int64_t contention_us;
static bool absent, command_pending, race, checking_veto, run_veto_race, tx_failed;
static _Atomic bool waiting;
static pthread_t acquisition;
static bool recovery_test, recovery_silent, recovery_second_silent, recovery_burst, recovery_bad_poll;
static unsigned recovery_wakes, recovery_phase, recovery_race_phase;
static bool sleep_model, worst_jitter, jitter_done;
static int64_t leaf_activity, leaf_sleep_ms=500, first_window_tx, gap_end;

static uint8_t recovery_target = 3;
static _Atomic bool release_waiter;
static bool hold_waiter;
static void recovery_race_at(unsigned phase);

static void lock(void) { assert(!pthread_mutex_lock(&inventory_mutex)); }
static void unlock(void) { assert(!pthread_mutex_unlock(&inventory_mutex)); }
int64_t esp_timer_get_time(void) { return clock_us; }
static int64_t utc(void) { return 1792531200000LL; }
static uint8_t quality(bool *suspect) { *suspect = false; return 3; }
void vTaskDelay(TickType_t ms) { if (checking_veto) { unsigned before=writes; sensor_inventory_refresh(); assert(writes==before && sensor_inventory_wait_ms()==UINT32_MAX); } if (recovery_test && recovery_phase==1 && worst_jitter) {
        if (!gap_end) gap_end=clock_us+200000;
        if (!jitter_done && clock_us+(int64_t)ms*1000>=gap_end) {
            clock_us=first_window_tx+450000-(int64_t)ms*1000;jitter_done=true;
        }
    }
    clock_us += (int64_t)ms * 1000; waiting = true;
    if (recovery_test && hold_waiter && pthread_equal(pthread_self(), acquisition))
        while (!release_waiter) sched_yield();
    if (recovery_test && recovery_phase == 1) recovery_race_at(2);
    sched_yield(); }
static void yield(uint32_t ms) { vTaskDelay(ms); }
void identity_host_enter(portMUX_TYPE *mux) { (void)mux; }
void identity_host_exit(portMUX_TYPE *mux) { (void)mux; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { assert(mutex_count < 5); return &mutexes[mutex_count++]; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t p, TickType_t wait)
{ int *held = p; if(contention_us) { clock_us+=contention_us;contention_us=0; } if (*held) { clock_us += (int64_t)wait * 1000; return 0; } *held = 1; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t p) { int *held = p; assert(*held); *held = 0; return pdTRUE; }
esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_set_level(int pin, unsigned value) { (void)pin; (void)value; return ESP_OK; }
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config) { assert(port == 0 && config); return ESP_OK; }
esp_err_t uart_driver_install(uart_port_t port, int r, int t, int q, void *h, int flags)
{ (void)r; (void)t; (void)q; (void)h; (void)flags; assert(port == 0); return ESP_OK; }
esp_err_t uart_set_pin(uart_port_t port, int tx, int r, int a, int b)
{ (void)tx; (void)r; (void)a; (void)b; assert(port == 0); return ESP_OK; }
esp_err_t uart_flush_input(uart_port_t port)
{ assert(port == 0); rx_size = rx_at = 0; return ESP_OK; }
static void *run(void *unused)
{ (void)unused; if(run_veto_race) sensor_inventory_run_begin();
  if (!run_veto_race || recovery_test) assert(sensor_inventory_acquire(recovery_test ? recovery_target : 3)); return NULL; }
static void recovery_race_at(unsigned phase)
{
    if (!recovery_race_phase || recovery_race_phase != phase) return;
    recovery_race_phase = 0; waiting = release_waiter = false; hold_waiter = true;
    assert(!pthread_create(&acquisition, NULL, run, NULL));
    while (!waiting) sched_yield();
}
static void append_rx(const void *bytes, size_t size)
{
    assert(rx_size + size <= sizeof rx);
    memcpy(rx + rx_size, bytes, size); rx_size += size;
}
int uart_write_bytes(uart_port_t port, const void *bytes, size_t size)
{
    assert(port == 0); const unsigned char *p = bytes; ++writes;
    if(tx_failed) return -1;
    if (recovery_test) {
        if (p[0] == 0xaa) {
            ++recovery_wakes;
            unsigned burst=(recovery_wakes-1)%3;
            recovery_phase=burst==0 ? 0 : burst==1 ? 1 : 3;
            if(recovery_phase==1) first_window_tx=clock_us;
            recovery_race_at(recovery_phase);
            if (!recovery_silent && !(recovery_second_silent && recovery_phase == 3)) {
                bool asleep=sleep_model && clock_us-leaf_activity>leaf_sleep_ms*1000;
                const uint8_t ack=asleep ? 0x85 : 0x80;append_rx(&ack,1);
                leaf_activity=clock_us;leaf_sleep_ms=asleep ? 1000 : 500;
                if (recovery_burst && recovery_phase == 3) {
                    const uint8_t stale[] = {0x80,0xa1}; append_rx(stale, sizeof stale);
                }
            }
        } else if (size == 1 && p[0] == 0xa0) command_pending = true;
        else if (command_pending && size == 8) {
            command_pending = false; assert(p[0] == 23);
            if (!recovery_silent && !recovery_second_silent && !recovery_burst) {
                const uint8_t answer[] = {0xa1, recovery_bad_poll ? 3 : 1, 0xf0};
                append_rx(answer, sizeof answer);
            }
        }
        return (int)size;
    }
    if (p[0] == 0xaa) {
        if (race) {
            race = false; assert(!pthread_create(&acquisition, NULL, run, NULL));
            while (!waiting) sched_yield();
        }
        if (!absent) { rx[0] = 0x80; rx_at = 0; rx_size = 1; }
    } else if (size == 1 && p[0] == 0xa0) command_pending = true;
    else if (command_pending && size == 8) {
        command_pending = false; rx_at = rx_size = 0;
        if (mode == 1) rx[rx_size++] = 0xf0;
        else if (mode != 2) {
            rx[rx_size++] = 0xa1;
            size_t count = p[0] != 33 ? 1 : p[1] == 2 ? 48 : p[1] == 1 ? 140 : 132;
            if (mode == 3) count--;
            if (mode == 4) count++;
            memcpy(rx + rx_size, payload, count); rx_size += count; rx[rx_size++] = 0xf0;
        }
    }
    return (int)size;
}
int uart_read_bytes(uart_port_t port, void *out, uint32_t size, TickType_t wait)
{
    assert(port == 0);
    if (rx_at == rx_size) { clock_us += (int64_t)wait * 1000; return 0; }
    size_t n = rx_size - rx_at; if (n > size) n = size;
    memcpy(out, rx + rx_at, n); rx_at += n; clock_us += 100; return (int)n;
}
static void started(uint8_t ch) { assert(ch < 4); ++refresh_starts; }
static void recovery_cases(void)
{
    const sensor_inventory_ports_t ports = {.now_us=esp_timer_get_time,.utc_ms=utc,.clock=quality,
        .lock=lock,.unlock=unlock,.yield_ms=yield,.inspect=uart_sensors_inspect,.recover=uart_sensors_recover};
    const uint8_t gains[6]={1,2,3,4,5,6}, currents[3]={7,8,9};
    unsigned char fw[48]={1,4}, cal[140]={0};fw[8]=2;
    sensor_snapshot_t before,after;
    recovery_test=true;run_veto_race=false;absent=false;mode=0;
    /* Both exchanges on the REAL driver, legacy fallback and exact poll. */
    for(unsigned supported=0;supported<2;++supported) {
        sensor_inventory_init(&ports,123);fw[1]=supported ? 4 : 0;fw[2]=6;
        assert(sensor_inventory_accept(0,2,fw,48));
        assert(sensor_inventory_accept(0,1,cal,140));sensor_inventory_config(0,gains,currents);
        assert(sensor_inventory_snapshot(&before));
        uint64_t epoch=sensor_inventory_acquire_run(0);sensor_inventory_run_window(0,epoch,1000);
        recovery_wakes=0;
        clock_us+=999000;sensor_inventory_tick();assert(!recovery_wakes && sensor_inventory_busy());
        clock_us+=1000;int64_t began=clock_us;sensor_inventory_tick();
        assert(clock_us-began<=900000 && recovery_wakes==3 && !sensor_inventory_busy());
        assert(sensor_inventory_snapshot(&after));assert(!memcmp(before.ports,after.ports,sizeof before.ports));
        assert(after.recovery_wire_transactions[0]==3 && !after.refresh.wire_transactions[0]);
    }
    /* Real idle-loop500/1000ms sleep: prime85 consumes waking bytes,
     * then both proof exchanges complete at the worst admitted450ms boundary. */
    sleep_model=worst_jitter=true;jitter_done=false;gap_end=0;leaf_activity=clock_us-2000000;
    sensor_inventory_init(&ports,123);assert(sensor_inventory_accept(0,2,fw,48));
    uint64_t sleeping_epoch=sensor_inventory_acquire_run(0);sensor_inventory_run_window(0,sleeping_epoch,0);
    recovery_wakes=0;sensor_inventory_tick();
    assert(!sensor_inventory_busy() && recovery_wakes==3 && jitter_done);
    assert(sensor_inventory_snapshot(&after) && after.recovery_boot_idle[0]==1);
    sleep_model=worst_jitter=false;gap_end=0;
    /* At least5 genuine UART-lock refusals consume no leaf attempt. */
    sensor_inventory_init(&ports,123);sleeping_epoch=sensor_inventory_acquire_run(0);
    sensor_inventory_run_window(0,sleeping_epoch,0);recovery_wakes=0;
    for(unsigned i=0;i<5;++i) {
        contention_us=800000;sensor_inventory_tick();
        assert(sensor_inventory_snapshot(&after) && after.busy==1 && !after.stalled && !recovery_wakes);
        assert(!after.recovery_wire_transactions[0]);clock_us+=60000000;
    }
    sensor_inventory_tick();assert(!sensor_inventory_busy() && recovery_wakes==3);
    /* Silence keeps ownership. No timer can release it, and no additional
     * attempts occur after the cap, even over a thousand idle seconds. */
    sensor_inventory_init(&ports,123);assert(sensor_inventory_accept(0,2,fw,48));
    assert(sensor_inventory_accept(0,1,cal,140));sensor_inventory_config(0,gains,currents);
    assert(sensor_inventory_snapshot(&before));
    uint64_t epoch=sensor_inventory_acquire_run(0);sensor_inventory_run_window(0,epoch,1000);
    recovery_silent=true;recovery_wakes=0;clock_us+=1000000;
    for(unsigned attempt=0;attempt<3;++attempt) {
        sensor_inventory_tick();assert(sensor_inventory_busy());
        assert(sensor_inventory_snapshot(&after) && !memcmp(before.ports,after.ports,sizeof before.ports));
        assert(after.recovery_wire_transactions[0]==attempt+1 && !after.refresh.wire_transactions[0]);
        if(attempt<2) clock_us+=(int64_t)60000000*(attempt+1);
    }
    assert(after.stalled==1);unsigned at_cap=recovery_wakes;
    clock_us+=1000000000;recovery_test=false;
    for(unsigned i=0;i<4;++i) { sensor_inventory_tick();clock_us+=250000; }
    assert(sensor_inventory_snapshot(&after) && after.stalled==1 && recovery_wakes==at_cap);
    assert(!after.refresh.wire_transactions[0]);
    assert(after.refresh.wire_transactions[1]+after.refresh.wire_transactions[2]+after.refresh.wire_transactions[3]>0);
    /* Explicit acquisition starts a new epoch/budget without a bank reset or
     * any change to the attachment/configuration. A clean proof clears stalled. */
    recovery_test=true;recovery_silent=false;recovery_wakes=0;
    uint64_t next_epoch=sensor_inventory_acquire_run(0);assert(next_epoch!=epoch);
    assert(sensor_inventory_snapshot(&after) && !after.stalled && after.busy==1);
    sensor_inventory_run_window(0,next_epoch,0);sensor_inventory_tick();
    assert(!sensor_inventory_busy() && recovery_wakes==3);
    assert(sensor_inventory_snapshot(&after) && !memcmp(&before.ports[0],&after.ports[0],sizeof before.ports[0]));
    /* Queued stale ACK outside a window is drained, never accepted. */
    recovery_test=true;sensor_inventory_init(&ports,123);recovery_wakes=0;
    epoch=sensor_inventory_acquire_run(0);sensor_inventory_run_window(0,epoch,0);
    const uint8_t stale=0x80;recovery_silent=true;append_rx(&stale,1);sensor_inventory_tick();
    assert(sensor_inventory_busy() && recovery_wakes==1);
    assert(sensor_inventory_snapshot(&after) && after.recovery_discarded_bytes[0]>0);
    /* A late ACK in a later window of the SAME epoch remains provisional;
     * only a second clean exchange permits release. */
    recovery_silent=false;clock_us+=60000000;recovery_wakes=0;sensor_inventory_tick();
    assert(!sensor_inventory_busy() && recovery_wakes==3);
    for(unsigned failure=0;failure<3;++failure) {
        sensor_inventory_init(&ports,123);assert(sensor_inventory_accept(0,2,fw,48));
        epoch=sensor_inventory_acquire_run(0);sensor_inventory_run_window(0,epoch,0);
        recovery_wakes=0;recovery_second_silent=failure==0;recovery_burst=failure==1;recovery_bad_poll=failure==2;
        sensor_inventory_tick();assert(sensor_inventory_busy() && recovery_wakes==3);
        if(failure) assert(sensor_inventory_snapshot(&after) && after.recovery_discarded_bytes[0]>0);
    }
    recovery_second_silent=recovery_burst=recovery_bad_poll=false;
    /* Shared UART0 admission at every proof phase. Pause the waiting task's
     * fake clock so scheduling does not invent elapsed wall time. */
    for(unsigned phase=1;phase<=3;++phase) {
        sensor_inventory_init(&ports,123);epoch=sensor_inventory_acquire_run(2);sensor_inventory_run_window(2,epoch,0);
        recovery_wakes=0;recovery_phase=0;recovery_race_phase=phase;recovery_target=3;run_veto_race=true;
        sensor_inventory_tick();release_waiter=true;assert(!pthread_join(acquisition,NULL));hold_waiter=false;
        assert(sensor_inventory_snapshot(&after) && after.busy==8);
        assert(after.refresh.run_start_waits==1 && after.refresh.run_start_wait_max_us>0 && after.refresh.run_start_wait_max_us<=1000000);
        sensor_inventory_veto_end();sensor_inventory_complete(3);
    }
    /* A new same-channel trigger changes epoch BEFORE it waits for the old
     * UART lease. The old window's physical ACK is discarded. */
    sensor_inventory_init(&ports,123);epoch=sensor_inventory_acquire_run(2);sensor_inventory_run_window(2,epoch,0);
    recovery_wakes=0;recovery_phase=0;recovery_race_phase=1;recovery_target=2;run_veto_race=false;
    sensor_inventory_tick();release_waiter=true;assert(!pthread_join(acquisition,NULL));hold_waiter=false;
    assert(sensor_inventory_snapshot(&after) && after.busy==4 && sensor_inventory_run_epoch(2)!=epoch);
    assert(after.refresh.run_start_waits==1 && after.refresh.run_start_wait_max_us<=1000000);
    assert(after.recovery_discarded_bytes[2]>0);
    sensor_inventory_complete(2);
#if CONFIG_AMBYTE_SENSOR_RECOVERY_HIL
    sensor_inventory_init(&ports,123);assert(sensor_inventory_accept(0,2,fw,48));
    assert(sensor_inventory_accept(0,1,cal,140));sensor_inventory_config(0,gains,currents);
    assert(sensor_inventory_snapshot(&before));sleep_model=true;recovery_wakes=0;
    for(unsigned n=0;n<20;++n) {
        clock_us+=1200000;
        assert(sensor_inventory_recovery_hil(0)==SENSOR_RECOVERY_PROVEN && !sensor_inventory_busy());
    }
    assert(sensor_inventory_snapshot(&after) && after.recovery_boot_idle[0]==20 && after.recovery_wire_transactions[0]==60);
    assert(!after.refresh.wire_transactions[0] && !memcmp(&before.ports[0],&after.ports[0],sizeof before.ports[0]));
    sleep_model=false;recovery_silent=true;
    assert(sensor_inventory_recovery_hil(0)==SENSOR_RECOVERY_FAILED && !sensor_inventory_busy());
    contention_us=800000;
    assert(sensor_inventory_recovery_hil(0)==SENSOR_RECOVERY_REFUSED && !sensor_inventory_busy());
    sensor_inventory_run_begin();
    epoch=sensor_inventory_acquire_run(0);sensor_inventory_run_window(0,epoch,60000);
    assert(sensor_inventory_recovery_hil(0)==SENSOR_RECOVERY_FAILED && sensor_inventory_busy());
    assert(sensor_inventory_snapshot(&after) && !after.stalled && !memcmp(&before.ports[0],&after.ports[0],sizeof before.ports[0]));
    sensor_inventory_veto_end();sensor_inventory_complete(0);recovery_silent=false;
    puts("RECOVERY_HIL_OK idle_blocks=20 boot_idle=20 active_silent=1");
#endif
    recovery_test=false;
    puts("RECOVERY_OK two_exchanges=1 stalled=1 port_scope=1 stale_burst=1 epoch=1 admission_phases=3");
}
int main(void)
{
    assert(uart_sensors_init() == ESP_OK); clock_us = 6000000;
    const sensor_inventory_ports_t ports = {.now_us = esp_timer_get_time, .utc_ms = utc, .clock = quality,
        .lock = lock, .unlock = unlock, .yield_ms = yield, .inspect = uart_sensors_inspect};
    sensor_inventory_init(&ports, 123);
    memset(payload, 0, sizeof payload); payload[0] = 1; payload[1] = 4; payload[8] = 1;
    unsigned char out[140]; size_t got;
    assert(uart_sensors_inspect(0, 2, out, 48, &got, 900, started) == SENSOR_READ_OK && got == 48);
    for (mode = 1; mode <= 4; ++mode) {
        int64_t before = clock_us;
        sensor_read_result_t result = uart_sensors_inspect(0, 4, out, 132, &got, 900, started);
        assert(result != SENSOR_READ_OK && clock_us - before <= 1000000);
        unsigned old_mode = mode; mode = 0;
        assert(uart_sensors_inspect(0, 2, out, 48, &got, 900, started) == SENSOR_READ_OK);
        assert(got == 48 && !memcmp(out, payload, 48)); mode = old_mode;
    }
    mode = 0;
    absent=true;contention_us=800000;
    assert(uart_sensors_inspect(0,0,NULL,0,&got,900,started)==SENSOR_READ_FAILED);
    tx_failed=true;unsigned no_tx_before=refresh_starts;
    assert(uart_sensors_inspect(0,0,NULL,0,&got,900,started)==SENSOR_READ_FAILED);
    assert(refresh_starts==no_tx_before);tx_failed=false;absent=false;
    unsigned previous = refresh_starts;
    uart_sensor_query_fn query = uart_sensors_get_query_fn();
    const unsigned opcodes[] = {20,21,22,23,24,25,26,27,33};
    for (unsigned i = 0; i < sizeof opcodes / sizeof opcodes[0]; ++i) {
        uint8_t cmd[8] = {(uint8_t)opcodes[i], 2}; uart_sensor_response_t response = {0};
        assert(query(0, cmd, NULL, 0, opcodes[i] == 33 ? 48 : 1, &response, 900) == ESP_OK);
        uart_sensor_response_free(&response); assert(refresh_starts == previous);
    }
    assert(uart_sensors_flash_session_begin(0, 100) == ESP_OK);
    unsigned before_veto = writes; sensor_inventory_refresh();assert(writes==before_veto);
    checking_veto = true;
    assert(uart_sensors_enter_download(0) == ESP_OK); assert(uart_sensors_run_app(0) == ESP_OK);
    uart_sensors_flash_session_end(0); checking_veto = false; assert(refresh_starts == previous);
    /* Actual refresh-origin path proves its counter is live. */
    sensor_inventory_refresh(); sensor_snapshot_t snapshot; assert(sensor_inventory_snapshot(&snapshot));
    assert(snapshot.refresh.wire_transactions[0] == 1);
    absent = true; race = true; waiting = false; clock_us += 250000;
    sensor_inventory_refresh(); assert(!pthread_join(acquisition, NULL));
    assert(sensor_inventory_snapshot(&snapshot));
    assert(snapshot.refresh.run_start_waits == 1 && snapshot.refresh.run_start_wait_max_us <= 1000000);
    assert(snapshot.busy == 8); sensor_inventory_complete(3);
    assert(writes && refresh_starts == previous);
    /* A real legacy timeout sets the raw driver disconnected while the owned
     * run retains the full cache and configuration. */
    assert(sensor_inventory_accept(0, 2, payload, 48));
    unsigned char cal[140]={0};assert(sensor_inventory_accept(0,1,cal,140));
    const uint8_t gains[6]={1,2,3,4,5,6}, currents[3]={7,8,9};sensor_inventory_config(0,gains,currents);
    sensor_snapshot_t saved;assert(sensor_inventory_snapshot(&saved));
    assert(sensor_inventory_acquire(0));
    uint8_t cmd[8]={23};uart_sensor_response_t response={0};
    assert(query(0,cmd,NULL,0,1,&response,900)!=ESP_OK);uart_sensor_response_free(&response);
    unsigned before_busy=writes; sensor_inventory_refresh();assert(writes==before_busy);
    assert(sensor_inventory_snapshot(&snapshot));assert(!memcmp(saved.ports,snapshot.ports,sizeof saved.ports));
    sensor_inventory_complete(0);
    checking_veto=true;assert(uart_sensors_reset_all(1000)==ESP_OK);checking_veto=false;
    assert(sensor_inventory_wait_ms()!=UINT32_MAX);
    /* The actual runner admission waits before triggering. Exercise every
     * refresh phase so its counter cannot stay zero under whole-job vetoes. */
    absent=false;run_veto_race=true;
    for(unsigned phase=0;phase<4;++phase) {
        sensor_inventory_init(&ports,123);
        for(unsigned i=0;i<phase*4;++i) {
            uint32_t wait=sensor_inventory_wait_ms();assert(wait!=UINT32_MAX);
            clock_us+=(int64_t)wait*1000;sensor_inventory_refresh();
        }
        uint32_t wait=sensor_inventory_wait_ms();clock_us+=(int64_t)wait*1000;
        race=true;waiting=false;sensor_inventory_refresh();assert(!pthread_join(acquisition,NULL));
        assert(sensor_inventory_snapshot(&snapshot));
        assert(snapshot.refresh.run_start_waits==1 && snapshot.refresh.run_start_wait_max_us>0);
        assert(snapshot.refresh.run_start_wait_max_us<=1000000 && sensor_inventory_wait_ms()==UINT32_MAX);
        sensor_inventory_veto_end();
    }
    recovery_cases();
    puts("UART_SENSOR_OK strict_frames=1 deadline=1 refresh_origin_only=1 shared_bank_race=1");
    return 0;
}

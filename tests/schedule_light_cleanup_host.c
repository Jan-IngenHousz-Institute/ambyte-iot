#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sched_runner_priv.h"
#include "ambit_trace.h"
#include "ambit_protocol.h"
#include "uart_stream_support.h"

#define pdTRUE 1
#define ESP_ERR_TIMEOUT 0x107
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
#define AMBIT_APP_BOOT_GRACE_US 5000000
#define TAG "test"
/* Consume arguments, as real logging does, without test noise. */
#define ESP_LOGI(tag, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) ESP_LOGI(tag, __VA_ARGS__)

static bool s_inited = true, stopped, present = true, stop_on_trigger, lost_ack;
static bool led, autonomous, held[4];
static int s_ch_mtx[4] = {0, 1, 2, 3};
static int busy_lock = -1, resets, pulses, triggers, fetches, take_count;
static int64_t clock_us = 1000000;
static uint8_t last_cmd[8];
static int64_t now_us(void) { return clock_us; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool sched_runner_should_stop(void) { return stopped; }
static void vTaskDelay(uint32_t ms) { clock_us += (int64_t)ms * 1000; }
static int xSemaphoreTake(int lock, uint32_t ticks)
{
    take_count++;
    if (lock == busy_lock) { clock_us += (int64_t)ticks * 1000; busy_lock = -1; return 0; }
    assert(!held[lock]); held[lock] = true; return pdTRUE;
}
static void xSemaphoreGive(int lock) { assert(held[lock]); held[lock] = false; }
static esp_err_t uart_sensors_run_app(uint8_t ch)
{
    assert(ch == 0);
    for (int i = 0; i < 4; i++) assert(held[i]);
    resets++; autonomous = false; led = false;
    return ESP_OK;
}
static cmd_result_t ambit_action(uint8_t ch, const uint8_t *cmd,
                                 const uint8_t *extra, size_t len, uint32_t timeout)
{
    (void)ch; (void)extra; (void)len; (void)timeout;
    memcpy(last_cmd, cmd, sizeof(last_cmd));
    assert(cmd[0] == AMBIT_CMD_ACTINIC);
    /* Existing AMBIT cmd 4 turns off first, but type 5 enables even at zero
     * current (AS7341 clamps to 4 mA). Count any ON transition, not final state. */
    led = false;
    if (cmd[1] == 5) { pulses++; led = true; led = false; }
    return (cmd_result_t){.status = ESP_OK};
}
cmd_result_t cmd_uart_ping(uint8_t ch, bool *connected)
{
    *connected = present && ch == 0;
    return (cmd_result_t){.status = ESP_OK};
}
void device_commands_measurement_begin(void) {}
void device_commands_measurement_end(void) {}
int64_t ambit_trace_estimate_ms(const ambit_trace_segment_t *segs, size_t n)
{ (void)segs; (void)n; return 1; }
cmd_result_t ambit_trace_trigger(uint8_t ch, const ambit_trace_segment_t *segs,
        size_t n, const ambit_trace_options_t *opts, ambit_trace_pending_t *pending)
{
    (void)ch; (void)segs; (void)n; (void)opts; (void)pending;
    triggers++; led = true; autonomous = true;
    if (stop_on_trigger) stopped = true;
    return (cmd_result_t){.status = lost_ack ? ESP_ERR_TIMEOUT : ESP_OK};
}
cmd_result_t cmd_ambit_poll(uint8_t ch, uint8_t *state, uint32_t timeout)
{
    (void)ch; (void)timeout; *state = AMBIT_ASYNC_DONE;
    return (cmd_result_t){.status = ESP_OK};
}
cmd_result_t ambit_trace_fetch(uint8_t ch, ambit_trace_pending_t *pending,
        bool store, uint32_t timeout, ambit_trace_result_t *out)
{
    (void)ch; (void)pending; (void)store; (void)timeout;
    fetches++; autonomous = false; memset(out, 0, sizeof(*out));
    return (cmd_result_t){.status = ESP_OK};
}

#include "production.inc"

static void trace(bool persist)
{
    static sched_program_t prog;
    const char *yaml =
        "schema: jii.ambyte-schedule/v1-draft\nname: test\nprotocols:\n  test:\n    persist: true\n    segments:\n"
        "      - pulses: 1\n        freq: 1\n        actinic: 500\n"
        "jobs:\n  test:\n    schedule:\n      cron: \"* * * * *\"\n    steps:\n"
        "      - uses: ambit/trace\n        with:\n          protocol: test\n";
    char error[256];
    esp_err_t err = sched_compile_text(yaml, strlen(yaml), &prog, error, sizeof(error));
    if (err != ESP_OK) { fprintf(stderr, "%s\n", error); assert(err == ESP_OK); }
    prog.protocols[0].persist = persist;
    sched_runner_act_ctx_t ctx = {.job_name = "test"};
    (void)act_ambit_trace(&ctx, &prog.jobs[0].steps[0], &prog);
}

int main(void)
{
    /* Zero-current requests must never enable light, including the ordinary
     * 1 s action and its extra 100 ms cleanup. Positive pulses still pass. */
    cmd_ambit_actinic(0, 5, 0, 10);
    cmd_ambit_actinic(0, 5, 0, 1);
    assert(pulses == 0 && last_cmd[1] == 0 && last_cmd[3] == 0);
    cmd_ambit_actinic(0, 5, 40, 10);
    assert(pulses == 1 && last_cmd[1] == 5 && last_cmd[2] == 40 && last_cmd[3] == 10);
    cmd_ambit_actinic(0, 2, 0, 12);
    assert(last_cmd[1] == 2 && last_cmd[3] == 12); /* calibration unchanged */

    /* A stop during the first trigger and a lost ACK both require reset. */
    for (int fail = 0; fail < 2; fail++) {
        stopped = false; stop_on_trigger = true; lost_ack = fail;
        trace(true);
        assert(led && autonomous && s_persist_attempted);
        /* Busy negative ping cache must not block cleanup. */
        present = false;
        int before = resets;
        sched_runner_cleanup_lights();
        assert(resets == before + 1 && !led && !autonomous && !s_persist_attempted);
        sched_runner_cleanup_lights();
        assert(resets == before + 1); /* idempotent for this generation */
        present = true;
    }
    /* Successful fetch leaves persistent light; stopping while idle cleans it.
     * A later nonpersistent attempt does not erase the earlier obligation. */
    stopped = false; stop_on_trigger = false; lost_ack = false;
    trace(true);
    assert(fetches == 1 && led && !autonomous);
    trace(false);
    stopped = true;
    int before = resets;
    busy_lock = 2; /* retry after lock timeout; partial locks must be released */
    sched_runner_cleanup_lights();
    assert(resets == before + 1 && !led);
    for (int i = 0; i < 4; i++) assert(!held[i]);

    /* Fresh nonpersistent generation or absent channels need no bank reset. */
    stopped = false; stop_on_trigger = true;
    trace(false);
    before = resets; sched_runner_cleanup_lights(); assert(resets == before);
    stopped = false; present = false;
    trace(true);
    sched_runner_cleanup_lights(); assert(resets == before);

    /* Each possible contended channel rolls back without touching hardware;
     * retry succeeds only after all four are owned. */
    for (int ch = 0; ch < 4; ch++) {
        busy_lock = ch; before = resets; take_count = 0;
        int64_t start = now_us();
        assert(uart_sensors_reset_all(1000) == ESP_ERR_TIMEOUT);
        assert(now_us() - start == 1000000 && resets == before && take_count == ch + 1);
        for (int i = 0; i < 4; i++) assert(!held[i]);
        assert(uart_sensors_reset_all(1000) == ESP_OK);
        assert(now_us() - start == 6000000 && resets == before + 1);
    }
    s_inited = false; before = resets;
    assert(uart_sensors_reset_all(1000) == ESP_ERR_INVALID_STATE && resets == before);
    puts("SCHEDULE_LIGHT_CLEANUP_OK");
}

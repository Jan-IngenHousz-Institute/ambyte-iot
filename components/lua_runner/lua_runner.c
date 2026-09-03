#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ambit_trace.h"
#include "device_commands.h"
#include "event_log.h"
#include "payload_v3.h"
#include "ambit_protocol.h"   /* AMBIT_ASYNC_* run-state constants for ambit.poll */
#include "time_sync.h"
#include "timezone.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "lua_runner.h"

#define LUA_RUNNER_TAG "lua_runner"
#define LUA_RUNNER_TASK_NAME "lua_runner"
#define LUA_RUNNER_TASK_STACK 8192
/* Measurement + local storage runs above all application comms tasks so a
 * scheduled measurement fires promptly and its SQLite write is prioritised over
 * MQTT publishing (esp-mqtt task = 5, sync_runner = 3). Kept WELL below the
 * network stack — LwIP TCP/IP (18) and Wi-Fi (~23) must stay higher or the
 * radio link breaks; those tasks are bursty/idle so they don't slow us. */
#define LUA_RUNNER_TASK_PRIORITY 10
/* Pin to APP_CPU (core 1). Wi-Fi (pinned core 0) and LwIP (prio 18) outrank the
 * measurement (prio 10), so an unpinned Lua task that lands on core 0 gets
 * preempted mid-UART-transaction → FSM/timing jitter. Pinning the timing-
 * sensitive measurement to core 1 keeps it off the radio's core; the slack
 * when it blocks is still reused by other tasks allowed on core 1. (ESP32-S3 is
 * dual-core; see docs/measurement-flow-plan.md.) */
#define LUA_RUNNER_TASK_CORE 1
#define LUA_QUERY_MAX_RECORDS 64
/* Canonical script home is INTERNAL flash (littlefs): the script arrives via
 * flashing or MQTT script_update, so the SD card — the old home — is only a
 * manual recovery source for offline units (see lua_runner_import_script). An
 * SD pull/corruption can no longer stop the measurement loop. */
#define LUA_SCRIPT_PATH     "/littlefs/main.lua"
#define LUA_SCRIPT_PATH_NEW "/littlefs/main.lua.new"
#define LUA_SCRIPT_PATH_BAK "/littlefs/main.lua.bak"

/* Bundled scheduler library (components/lua_runner/sched.lua), generated into a
 * C byte array at configure time (see CMakeLists) and loaded before the user
 * script to define the `sched` global. Provides sched_lua_start[] + sched_lua_size. */
#include "sched_lua_embed.h"

static TaskHandle_t s_lua_task_handle = NULL;

/* Stop-flag inspected by the Lua debug hook + the interruptible sleep. Set by
 * lua_runner_stop(); the script unwinds via luaL_error and the task exits. */
static volatile bool s_should_stop = false;

/* GC request inspected by the Lua debug hook. Set (from another task) by
 * lua_runner_request_gc() so the MQTT publisher can ask the scripting VM to
 * collect + de-fragment the shared heap before a large publish. Serviced in the
 * Lua task's own context (the hook), which is the only safe place to touch L. */
static volatile bool s_gc_request = false;

/* Signaled by the task when it has finished cleaning up (lua_close done). Lets
 * lua_runner_stop() wait for a clean exit before lua_runner_start() spawns a
 * new task — avoids two Lua states alive at once. Created lazily. */
static SemaphoreHandle_t s_done_sem = NULL;
static StaticSemaphore_t s_done_sem_storage;

/* ── helpers ─────────────────────────────────────────────────────────── */

static int lua_push_nil_reason(lua_State *L, const char *reason)
{
    lua_pushnil(L);
    lua_pushstring(L, (reason != NULL) ? reason : "unknown error");
    return 2;
}

/* ── device.* bindings ───────────────────────────────────────────────── */
/* NOTE: there is deliberately no device.set_rgb. The status LED is owned by
 * the firmware blinker (ambyte_status), which encodes device state; a script
 * driving the LED would fight it. Bench debugging keeps the CLI `set_rgb`. */

static int l_device_read_rtc(lua_State *L)
{
    time_t t = 0;
    cmd_result_t res = cmd_read_rtc(&t);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}

static int l_device_status(lua_State *L)
{
    bool bme_ready = false;
    bool rtc_ready = false;
    time_t rtc_time = 0;
    cmd_result_t res = cmd_device_status(&bme_ready, &rtc_ready, &rtc_time);

    lua_newtable(L);
    lua_pushboolean(L, bme_ready);
    lua_setfield(L, -2, "bme280");
    lua_pushboolean(L, rtc_ready);
    lua_setfield(L, -2, "rtc");
    if (rtc_ready) {
        lua_pushinteger(L, (lua_Integer)rtc_time);
        lua_setfield(L, -2, "rtc_time");
    }
    lua_pushstring(L, res.message);
    lua_setfield(L, -2, "summary");
    return 1;
}

/* Wall-clock epoch milliseconds (RTC-backed system clock). */
static int64_t lua_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* opts.store at stack index `idx` — default true (fused-store convention:
 * measurement commands store unless the script opts out). */
static bool lua_opt_store(lua_State *L, int idx)
{
    bool store = true;
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "store");
        if (!lua_isnil(L, -1)) store = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    return store;
}

/* device.bme280([opts]) — read the on-board BME280 and (by default) store one
 * canonical ambyte.telemetry/1 event (channel null, cmd_raw device.bme280).
 *   opts: { store = true }   -- pass store=false for a read-only probe
 * Returns { temperature_c, humidity_pct, pressure_pa [, id] } or nil,err;
 * `id` is the stored measure_id (absent when store=false or store failed). */
static int l_device_bme280(lua_State *L)
{
    bool store = lua_opt_store(L, 1);

    float t = 0, h = 0, p = 0;
    int64_t mid = -1;
    if (store) {
        int64_t id = 0;
        measurement_t m;
        cmd_result_t res = cmd_record_env(&id, &m);
        if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
        t = m.temperature_c;
        h = m.humidity_percent;
        p = m.pressure_pa;
        mid = id;
    } else {
        cmd_result_t res = cmd_read_env(&t, &h, &p);
        if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    }

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)t); lua_setfield(L, -2, "temperature_c");
    lua_pushnumber(L, (lua_Number)h); lua_setfield(L, -2, "humidity_pct");
    lua_pushnumber(L, (lua_Number)p); lua_setfield(L, -2, "pressure_pa");
    if (mid >= 0) {
        lua_pushinteger(L, (lua_Integer)mid);
        lua_setfield(L, -2, "id");
    }
    return 1;
}

/* device.power() → { battery_v, input_v, system_v, input_ma, charge_ma }
 * or nil,err. Reads the MP2731 charger telemetry (voltages in volts, currents
 * in mA). */
static int l_device_power(lua_State *L)
{
    power_reading_t p;
    cmd_result_t res = cmd_read_power(&p);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)p.battery_mv / 1000.0);
    lua_setfield(L, -2, "battery_v");
    lua_pushnumber(L, (lua_Number)p.input_mv / 1000.0);
    lua_setfield(L, -2, "input_v");
    lua_pushnumber(L, (lua_Number)p.system_mv / 1000.0);
    lua_setfield(L, -2, "system_v");
    lua_pushinteger(L, (lua_Integer)p.input_ma);
    lua_setfield(L, -2, "input_ma");
    lua_pushinteger(L, (lua_Integer)p.charge_ma);
    lua_setfield(L, -2, "charge_ma");
    lua_pushboolean(L, p.input_present);
    lua_setfield(L, -2, "input_present");
    lua_pushboolean(L, p.charge_status != 0);
    lua_setfield(L, -2, "charging");
    return 1;
}

/* device.sd_ready() -> true|false. Gate measurement rounds with this so the
 * loop pauses while the SD card is out (DB closed) and resumes on reinsert. */
static int l_device_sd_ready(lua_State *L)
{
    bool ready = false;
    cmd_result_t res = cmd_sd_ready(&ready);
    (void)res;     /* not_supported / errors → ready stays false, push as-is */
    lua_pushboolean(L, ready ? 1 : 0);
    return 1;
}

static int l_device_sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0 || ms > INT_MAX) {
        return luaL_error(L, "ms must be in [0, INT_MAX]");
    }
    /* Interruptible sleep: chop into 100 ms chunks so a stop signal wakes the
     * task quickly instead of waiting for a full 30 s sleep to elapse. */
    const uint32_t CHUNK_MS = 100;
    uint32_t remaining = (uint32_t)ms;
    while (remaining > 0) {
        if (s_should_stop) {
            return luaL_error(L, "lua_runner: stop signaled during sleep");
        }
        uint32_t step = remaining > CHUNK_MS ? CHUNK_MS : remaining;
        cmd_sleep_ms(step);
        remaining -= step;
    }
    return 0;
}

static int l_device_log(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    cmd_log(msg);
    return 0;
}

/* device.measurement_window(on) — assert/release measurement activity around a
 * whole cycle. This retains the PM no-light-sleep lock and telemetry semantics;
 * publishing is held only by nested raw sensor transactions unless the legacy
 * compile-time gate is enabled. Pair every on with exactly one off. */
static int l_device_measurement_window(lua_State *L)
{
    if (lua_toboolean(L, 1)) device_commands_measurement_begin();
    else                     device_commands_measurement_end();
    return 0;
}

/* device.uptime_ms() — monotonic milliseconds since boot, for elapsed-time /
 * deadline math (unaffected by RTC/wall-clock changes). esp_log_timestamp()
 * needs no extra component dependency and wraps only after ~49 days. */
static int l_device_uptime_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)esp_log_timestamp());
    return 1;
}

/* device.status_report() → table | nil,err. Point-in-time device state (Wi-Fi,
 * provisioning, event DB, MP2731 power + source/charge/publish-gate). Intended
 * to be stored via db.store_event{ data = ... } so the sole publisher
 * (sync_runner) forwards it under the power gate. (Becomes a firmware
 * background task with tag=STATUS in payload-v2 Phase 4.) */
static int l_device_status_report(lua_State *L)
{
    device_status_snapshot_t s;
    cmd_result_t res = cmd_status_report(&s);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }

    lua_newtable(L);
    lua_pushboolean(L, s.wifi_connected);    lua_setfield(L, -2, "wifi");
    lua_pushboolean(L, s.provisioned);       lua_setfield(L, -2, "provisioned");
    lua_pushboolean(L, s.db_online);         lua_setfield(L, -2, "db_online");
    lua_pushboolean(L, s.publish_gate_open); lua_setfield(L, -2, "publish_gate");
    if (s.power_valid) {
        lua_pushnumber(L, (lua_Number)s.power.battery_mv / 1000.0);
        lua_setfield(L, -2, "battery_v");
        lua_pushnumber(L, (lua_Number)s.power.input_mv / 1000.0);
        lua_setfield(L, -2, "input_v");
        lua_pushnumber(L, (lua_Number)s.power.system_mv / 1000.0);
        lua_setfield(L, -2, "system_v");
        lua_pushinteger(L, (lua_Integer)s.power.input_ma);
        lua_setfield(L, -2, "input_ma");
        lua_pushinteger(L, (lua_Integer)s.power.charge_ma);
        lua_setfield(L, -2, "charge_ma");
        lua_pushboolean(L, s.power.input_present);
        lua_setfield(L, -2, "input_present");
        lua_pushinteger(L, (lua_Integer)s.power.charge_status);
        lua_setfield(L, -2, "charge_status");
    }
    return 1;
}

/* device.PWM(duty, freq, enable)
 *   duty   0..100  float, default 0
 *   freq   Hz,     default 10000
 *   enable boolean default true; pass false to stop output (pin held low)
 * Returns the status message string; raises on invalid args / HW error. */
static int l_device_pwm(lua_State *L)
{
    lua_Number  duty   = luaL_optnumber(L, 1, 0.0);
    lua_Integer freq   = luaL_optinteger(L, 2, 10000);
    bool        enable = lua_isnoneornil(L, 3) ? true : (bool)lua_toboolean(L, 3);

    if (duty < 0.0 || duty > 100.0) {
        return luaL_error(L, "duty must be in [0, 100]");
    }
    if (enable && freq <= 0) {
        return luaL_error(L, "freq must be > 0");
    }

    cmd_result_t res = cmd_pwm((float)duty, (uint32_t)freq, enable);
    if (res.status != ESP_OK) {
        return luaL_error(L, "%s", res.message);
    }
    lua_pushstring(L, res.message);
    return 1;
}

/* ── uart transport + ambit presence bindings ───────────────────────── */

/* ambit.ping(ch) → boolean — AMBIT wake (0xAA) / ack (0x80) probe */
static int l_device_uart_ping(lua_State *L)
{
    lua_Integer ch = luaL_checkinteger(L, 1);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    }
    bool connected = false;
    cmd_result_t res = cmd_uart_ping((uint8_t)ch, &connected);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushboolean(L, connected);
    return 1;
}

/* uart.status() → string summary of each channel's connection state */
static int l_device_uart_status(lua_State *L)
{
    cmd_result_t res = cmd_uart_status();
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushstring(L, res.message);
    return 1;
}

/* uart.query(ch, cmd, timeout_ms [, opts]) — ASCII line-oriented query.
 * Raw transport for not-yet-drivered sensors; NEVER stores (schema-v2 rule:
 * measurement commands store, transport commands don't).
 *
 *   ch          : channel index 0..3
 *   cmd         : ASCII command string (terminator appended by the runtime)
 *   timeout_ms  : total budget for send + read in ms
 *   opts        : optional table: lineterminator = "\n" (default)
 *
 * Returns the response string (terminator stripped) or "" on timeout. On a
 * hard error returns nil, err. Echo handling: if the first line read back
 * equals `cmd` verbatim, it is dropped and the next line is returned. */
static int l_uart_query(lua_State *L)
{
    lua_Integer ch = luaL_checkinteger(L, 1);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    }
    const char *cmd        = luaL_checkstring(L, 2);
    lua_Integer timeout_ms = luaL_checkinteger(L, 3);

    const char *terminator = "\n";
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "lineterminator");
        if (lua_isstring(L, -1)) {
            terminator = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    char   response[256];
    size_t resp_len = 0;
    cmd_result_t res = cmd_uart_text_query((uint8_t)ch, cmd, terminator,
                                           (uint32_t)timeout_ms,
                                           response, sizeof(response), &resp_len);

    /* Hard error (anything other than success or pure timeout). */
    if (res.status != ESP_OK && res.status != ESP_ERR_TIMEOUT) {
        return lua_push_nil_reason(L, res.message);
    }

    /* On both success and timeout, return the response (possibly empty). */
    lua_pushlstring(L, response, resp_len);
    return 1;
}

/* ambit.query(channel, cmd_bytes, extra_bytes_or_nil, expect_raw, timeout_ms)
 *
 * AMBIT binary protocol — sends the 8-byte ESP command with optional `extra`
 * payload, waits for CMD_DONE + either a fixed-size raw response or the FSM
 * data-transfer arrays. See cmd_uart_query in device_commands.c.
 *
 *   cmd_bytes  : table of 8 integers (the 8-byte command)
 *   extra_bytes: string of extra payload bytes (or nil)
 *   expect_raw : integer — 0 for FSM mode, >0 for immediate raw response length
 *   timeout_ms : integer
 *
 * Returns:
 *   FSM mode  → { arrays = { {index=N, data={...}}, ... }, array_count=N }
 *   Raw mode  → { raw = "binary string", raw_len = N }
 *   Error     → nil, error_message
 */
static int l_ambit_uart_query(lua_State *L)
{
    lua_Integer ch         = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer expect_raw = luaL_checkinteger(L, 4);
    lua_Integer timeout_ms = luaL_checkinteger(L, 5);

    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    }

    /* Build 8-byte command from table */
    uint8_t cmd[8] = {0};
    for (int i = 1; i <= 8; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isinteger(L, -1)) {
            cmd[i - 1] = (uint8_t)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    /* Optional extra payload (string or nil at index 3) */
    const uint8_t *extra = NULL;
    size_t extra_len = 0;
    if (lua_isstring(L, 3)) {
        extra = (const uint8_t *)lua_tolstring(L, 3, &extra_len);
    }

    uart_sensor_response_t response;
    memset(&response, 0, sizeof(response));
    cmd_result_t res = cmd_uart_query((uint8_t)ch, cmd, extra, extra_len,
                                      (size_t)expect_raw, &response,
                                      (uint32_t)timeout_ms);
    if (res.status != ESP_OK) {
        uart_sensor_response_free(&response);
        return lua_push_nil_reason(L, res.message);
    }

    lua_newtable(L);

    if (expect_raw > 0 && response.raw != NULL) {
        /* Raw mode: push raw bytes as a Lua string */
        lua_pushlstring(L, (const char *)response.raw, response.raw_len);
        lua_setfield(L, -2, "raw");
        lua_pushinteger(L, (lua_Integer)response.raw_len);
        lua_setfield(L, -2, "raw_len");
    } else {
        /* FSM mode: push arrays table */
        lua_newtable(L);
        for (uint8_t i = 0; i < response.array_count; i++) {
            lua_newtable(L);
            lua_pushinteger(L, response.arrays[i].index);
            lua_setfield(L, -2, "index");

            /* Push data as a sub-table of integers */
            lua_newtable(L);
            for (uint16_t j = 0; j < response.arrays[i].length; j++) {
                lua_pushinteger(L, (lua_Integer)response.arrays[i].data[j]);
                lua_rawseti(L, -2, (int)(j + 1));
            }
            lua_setfield(L, -2, "data");

            lua_pushinteger(L, response.arrays[i].length);
            lua_setfield(L, -2, "length");

            lua_rawseti(L, -2, (int)(i + 1));
        }
        lua_setfield(L, -2, "arrays");
        lua_pushinteger(L, response.array_count);
        lua_setfield(L, -2, "array_count");
    }

    uart_sensor_response_free(&response);
    return 1;
}

/* ── ambit.* typed bindings ──────────────────────────────────────────── */

/* ambit.set_gains(ch, fluo, fluoref, ir, irref, sun, leaf) → true or nil,err */
static int l_device_ambit_set_gains(lua_State *L)
{
    uint8_t ch  = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t g[6];
    for (int i = 0; i < 6; i++) g[i] = (uint8_t)luaL_checkinteger(L, i + 2);
    cmd_result_t res = cmd_ambit_set_gains(ch, g[0], g[1], g[2], g[3], g[4], g[5]);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.set_currents(ch, i620, i720, ir) → true or nil,err */
static int l_device_ambit_set_currents(lua_State *L)
{
    uint8_t ch   = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t i620 = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t i720 = (uint8_t)luaL_checkinteger(L, 3);
    uint8_t ir   = (uint8_t)luaL_checkinteger(L, 4);
    cmd_result_t res = cmd_ambit_set_currents(ch, i620, i720, ir);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.config_detector(ch) → true or nil,err */
static int l_device_ambit_config_detector(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    cmd_result_t res = cmd_ambit_config_detector(ch);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.leaf_temp(ch [, opts]) — leaf + chip temperature (binary GET_TEMP,
 * AMBIT ASCII "get_temp"). Stores by default ({store=false} to probe);
 * payload {"leaf":..,"chip":..}.
 * Returns { leaf=float, chip=float [, id] } or nil,err. */
static int l_ambit_leaf_temp(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    bool store = lua_opt_store(L, 2);

    int64_t start_ms = lua_now_ms();
    float leaf = 0, chip = 0;
    cmd_result_t res = cmd_ambit_get_temp(ch, &leaf, &chip);
    int64_t end_ms = lua_now_ms();
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);

    int64_t mid = -1;
    if (store) {
        char payload[64];
        snprintf(payload, sizeof payload, "{\"leaf\":%.2f,\"chip\":%.2f}",
                 (double)leaf, (double)chip);
        ambit_device_info_t info;
        const char *dev = ambit_device_name(ch, &info);
        char meta[200];
        int o = snprintf(meta, sizeof meta, "{");
        o += ambit_config_kvs(&info, meta + o, sizeof meta - o);
        snprintf(meta + o, sizeof meta - o, "}");
        mid = ambit_store_small(ch, dev, "get_temp", meta, start_ms, end_ms, payload);
        if (mid < 0) ESP_LOGW(LUA_RUNNER_TAG, "ambit.leaf_temp ch%u: store failed", ch);
    }

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)leaf);  lua_setfield(L, -2, "leaf");
    lua_pushnumber(L, (lua_Number)chip);  lua_setfield(L, -2, "chip");
    if (mid >= 0) { lua_pushinteger(L, (lua_Integer)mid); lua_setfield(L, -2, "id"); }
    return 1;
}

/* ambit.spec(ch [, opts]) — spectrum + PAR (binary GET_SPEC, AMBIT ASCII
 * "get_par"). Stores by default ({store=false} to probe); payload
 * {"spec":[10 ints],"par":N}.
 * Returns { spec={10 ints}, par=float [, id] } or nil,err. */
static int l_ambit_spec(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    bool store = lua_opt_store(L, 2);

    int64_t start_ms = lua_now_ms();
    uint16_t spec[10] = {0};
    float par = 0;
    cmd_result_t res = cmd_ambit_get_spec(ch, spec, &par);
    int64_t end_ms = lua_now_ms();
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);

    int64_t mid = -1;
    if (store) {
        char payload[160];
        int n = snprintf(payload, sizeof payload,
                         "{\"spec\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"par\":%.2f}",
                         spec[0], spec[1], spec[2], spec[3], spec[4],
                         spec[5], spec[6], spec[7], spec[8], spec[9],
                         (double)par);
        if (n > 0 && n < (int)sizeof payload) {
            ambit_device_info_t info;
            const char *dev = ambit_device_name(ch, &info);
            char meta[200];
            int mo = snprintf(meta, sizeof meta, "{");
            mo += ambit_config_kvs(&info, meta + mo, sizeof meta - mo);
            snprintf(meta + mo, sizeof meta - mo, "}");
            mid = ambit_store_small(ch, dev, "get_par", meta, start_ms, end_ms, payload);
        }
        if (mid < 0) ESP_LOGW(LUA_RUNNER_TAG, "ambit.spec ch%u: store failed", ch);
    }

    lua_newtable(L);
    lua_newtable(L);
    for (int i = 0; i < 10; i++) {
        lua_pushinteger(L, spec[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "spec");
    lua_pushnumber(L, (lua_Number)par);
    lua_setfield(L, -2, "par");
    if (mid >= 0) { lua_pushinteger(L, (lua_Integer)mid); lua_setfield(L, -2, "id"); }
    return 1;
}

/* ambit.leaf_temp_raw(ch) → {leaf,leaf1,chip,raw={4 ints}} or nil,err
 * (diagnostic — never stores) */
static int l_device_ambit_get_temp_raw(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    float leaf = 0, leaf1 = 0, chip = 0;
    int16_t raw[4] = {0};
    cmd_result_t res = cmd_ambit_get_temp_raw(ch, &leaf, &leaf1, &chip, raw);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)leaf);   lua_setfield(L, -2, "leaf");
    lua_pushnumber(L, (lua_Number)leaf1);  lua_setfield(L, -2, "leaf1");
    lua_pushnumber(L, (lua_Number)chip);   lua_setfield(L, -2, "chip");
    lua_newtable(L);
    for (int i = 0; i < 4; i++) {
        lua_pushinteger(L, raw[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "raw");
    return 1;
}

/* ambit.get_info(ch, type) → string (raw bytes) or nil,err */
static int l_device_ambit_get_info(lua_State *L)
{
    uint8_t ch   = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t type = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t buf[256];
    size_t len = 0;
    cmd_result_t res = cmd_ambit_get_info(ch, type, buf, sizeof(buf), &len);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushlstring(L, (const char *)buf, len);
    return 1;
}

/* ambit.run_raw(ch, run_arr_table, led_persist, allow_interrupt, timeout_ms)
 *   run_arr_table: flat table of integers (length must be multiple of 8)
 *   Returns: response table (same as ambit.query FSM mode) or nil,err.
 *   Legacy/diagnostic — materialises arrays into Lua, never stores. */
static int l_device_ambit_run(lua_State *L)
{
    uint8_t ch         = (uint8_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    uint8_t led_persist     = (uint8_t)luaL_checkinteger(L, 3);
    bool    allow_interrupt = lua_toboolean(L, 4);
    uint32_t timeout_ms     = (uint32_t)luaL_checkinteger(L, 5);

    int n = (int)luaL_len(L, 2);
    if (n <= 0 || n > 128 || (n % 8) != 0) {
        return luaL_error(L, "run_arr must have 8-128 elements (multiple of 8)");
    }
    uint8_t arr_len = (uint8_t)(n / 8);
    uint8_t *run_arr = malloc((size_t)n);
    if (!run_arr) return luaL_error(L, "out of memory");
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        run_arr[i - 1] = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    uart_sensor_response_t response;
    cmd_result_t res = cmd_ambit_run(ch, run_arr, arr_len, led_persist,
                                      allow_interrupt, &response, timeout_ms);
    free(run_arr);
    if (res.status != ESP_OK) {
        uart_sensor_response_free(&response);
        return lua_push_nil_reason(L, res.message);
    }

    /* Push FSM arrays as table */
    lua_newtable(L);
    lua_newtable(L);
    for (uint8_t i = 0; i < response.array_count; i++) {
        lua_newtable(L);
        lua_pushinteger(L, response.arrays[i].index);
        lua_setfield(L, -2, "index");
        lua_newtable(L);
        for (uint16_t j = 0; j < response.arrays[i].length; j++) {
            lua_pushinteger(L, (lua_Integer)response.arrays[i].data[j]);
            lua_rawseti(L, -2, (int)(j + 1));
        }
        lua_setfield(L, -2, "data");
        lua_pushinteger(L, response.arrays[i].length);
        lua_setfield(L, -2, "length");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setfield(L, -2, "arrays");
    lua_pushinteger(L, response.array_count);
    lua_setfield(L, -2, "array_count");
    uart_sensor_response_free(&response);
    return 1;
}

/* ambit.run_mpf(ch, length, interval, change_act, act, timeout_ms) → table
 * (diagnostic — never stores) */
static int l_device_ambit_run_mpf(lua_State *L)
{
    uint8_t  ch         = (uint8_t)luaL_checkinteger(L, 1);
    uint16_t length     = (uint16_t)luaL_checkinteger(L, 2);
    uint8_t  interval   = (uint8_t)luaL_checkinteger(L, 3);
    bool     change_act = lua_toboolean(L, 4);
    uint8_t  act        = (uint8_t)luaL_checkinteger(L, 5);
    uint32_t timeout_ms = (uint32_t)luaL_checkinteger(L, 6);

    uart_sensor_response_t response;
    cmd_result_t res = cmd_ambit_run_mpf(ch, length, interval, change_act,
                                          act, &response, timeout_ms);
    if (res.status != ESP_OK) {
        uart_sensor_response_free(&response);
        return lua_push_nil_reason(L, res.message);
    }

    lua_newtable(L);
    lua_newtable(L);
    for (uint8_t i = 0; i < response.array_count; i++) {
        lua_newtable(L);
        lua_pushinteger(L, response.arrays[i].index);
        lua_setfield(L, -2, "index");
        lua_newtable(L);
        for (uint16_t j = 0; j < response.arrays[i].length; j++) {
            lua_pushinteger(L, (lua_Integer)response.arrays[i].data[j]);
            lua_rawseti(L, -2, (int)(j + 1));
        }
        lua_setfield(L, -2, "data");
        lua_pushinteger(L, response.arrays[i].length);
        lua_setfield(L, -2, "length");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setfield(L, -2, "arrays");
    lua_pushinteger(L, response.array_count);
    lua_setfield(L, -2, "array_count");
    uart_sensor_response_free(&response);
    return 1;
}

/* ambit.blink(ch, id, intensity) → true or nil,err */
static int l_device_ambit_blink(lua_State *L)
{
    uint8_t ch        = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t id        = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t intensity = (uint8_t)luaL_checkinteger(L, 3);
    cmd_result_t res = cmd_ambit_blink(ch, id, intensity);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.calibrate_baseline(ch) → true or nil,err */
static int l_device_ambit_calibrate_baseline(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    cmd_result_t res = cmd_ambit_calibrate_baseline(ch);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.actinic(ch, type, var, var2) → true or nil,err */
static int l_device_ambit_actinic(lua_State *L)
{
    uint8_t ch   = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t type = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t var  = (uint8_t)luaL_checkinteger(L, 3);
    uint8_t var2 = (uint8_t)luaL_checkinteger(L, 4);
    cmd_result_t res = cmd_ambit_actinic(ch, type, var, var2);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.set_metadata(ch, metadata_string) → true or nil,err */
static int l_device_ambit_set_metadata(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    cmd_result_t res = cmd_ambit_set_metadata(ch, (const uint8_t *)data, len);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ── db.* bindings ───────────────────────────────────────────────────── */

/* Recursively convert the Lua value at `idx` to a cJSON node. Tables with a
 * non-zero sequence length become JSON arrays; all other tables become JSON
 * objects (string keys only). Returns NULL on allocation failure. */
static cJSON *lua_to_cjson(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            return cJSON_CreateNumber((double)lua_tointeger(L, idx));
        return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        lua_len(L, idx);
        lua_Integer n = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (n > 0) {
            cJSON *arr = cJSON_CreateArray();
            if (!arr) return NULL;
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, i);
                cJSON *child = lua_to_cjson(L, -1);
                lua_pop(L, 1);
                if (child) cJSON_AddItemToArray(arr, child);
            }
            return arr;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) return NULL;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            /* key at -2, value at -1; only accept string keys */
            if (lua_type(L, -2) == LUA_TSTRING) {
                cJSON *child = lua_to_cjson(L, -1);
                if (child) cJSON_AddItemToObject(obj, lua_tostring(L, -2), child);
            }
            lua_pop(L, 1); /* pop value, keep key for next() */
        }
        return obj;
    }
    default:
        return cJSON_CreateNull();
    }
}

/* db.store_event{ data={...}, metadata=, channel= }
 *
 * Custom/derived events only — typed measurement commands (ambit.spec,
 * ambit.run, device.bme280, …) store themselves. Provenance is firmware-
 * filled: tag = MEASUREMENT, ticks = now, measure_id auto-allocated.
 *
 *   data      required table → JSON object of quantities (the payload)
 *   metadata  optional table → JSON object
 *   channel   optional integer 0..3 — attributes the event to a UART port
 *             ("uart_<n>"); omit for onboard/derived events (JSON null)
 *
 * Returns measure_id on success, or nil,reason. */
static int l_db_store_event(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* channel — optional integer → "uart_<n>" */
    char chan[12] = {0};
    lua_getfield(L, 1, "channel");
    if (lua_isinteger(L, -1) || lua_isnumber(L, -1)) {
        lua_Integer ch = lua_tointeger(L, -1);
        if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
            lua_pop(L, 1);
            return lua_push_nil_reason(L, "store_event: channel out of range");
        }
        snprintf(chan, sizeof chan, "uart_%d", (int)ch);
    }
    lua_pop(L, 1);

    int64_t measure_id = 0;
    cmd_result_t idr = cmd_next_measure_id(&measure_id);
    if (idr.status != ESP_OK) return lua_push_nil_reason(L, idr.message);

    const int64_t now = lua_now_ms();

    /* data — required table → payload JSON object */
    lua_getfield(L, 1, "data");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return lua_push_nil_reason(L, "store_event: 'data' (table) required");
    }
    cJSON *data = lua_to_cjson(L, -1);
    lua_pop(L, 1);
    if (data == NULL) return lua_push_nil_reason(L, "store_event: out of memory (data)");
    char *payload_json = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    if (payload_json == NULL) return lua_push_nil_reason(L, "store_event: payload encode failed");

    /* metadata — optional table → JSON object */
    char *metadata_json = NULL;
    lua_getfield(L, 1, "metadata");
    if (lua_istable(L, -1)) {
        cJSON *md = lua_to_cjson(L, -1);
        if (md) { metadata_json = cJSON_PrintUnformatted(md); cJSON_Delete(md); }
    }
    lua_pop(L, 1);

    measurement_event_desc_t d = {
        .measure_id    = measure_id,
        .channel       = chan,
        .tag           = MEASUREMENT_TAG_MEASUREMENT,
        .start_ms      = now,
        .end_ms        = now,
        .metadata_json = metadata_json,
        .payload_json  = payload_json,
    };
    cmd_result_t res = cmd_store_event(&d);
    cJSON_free(payload_json);
    if (metadata_json) cJSON_free(metadata_json);

    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushinteger(L, (lua_Integer)measure_id);
    return 1;
}

static int l_db_next_id(lua_State *L)
{
    int64_t id = 0;
    cmd_result_t res = cmd_next_measure_id(&id);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

/* NOTE: there is deliberately no Lua MQTT module. Publishing is owned solely by
 * sync_runner, which drains the event DB under the power gate. Lua scripts only
 * *store* (db.store_event); the runner forwards. This keeps the gate in one
 * place and makes the store the single, durable source of truth. */

/* ── module registration ─────────────────────────────────────────────── */

/* device.* — ONBOARD I/O only (schema-v2 namespacing: per-protocol driver
 * modules own their sensors; ambit.* owns everything AMBIT, uart.* is raw
 * transport). device.bme280 is a fused measurement command (stores by
 * default); the rest is I/O and diagnostics. */
static void lua_register_device_module(lua_State *L)
{
    static const luaL_Reg device_api[] = {
        {"read_rtc",               l_device_read_rtc},
        {"status",                 l_device_status},
        {"bme280",                 l_device_bme280},
        {"power",                  l_device_power},
        {"sd_ready",               l_device_sd_ready},
        {"sleep_ms",               l_device_sleep_ms},
        {"log",                    l_device_log},
        {"measurement_window",     l_device_measurement_window},
        {"uptime_ms",              l_device_uptime_ms},
        {"status_report",          l_device_status_report},
        {"PWM",                    l_device_pwm},
        {NULL, NULL},
    };

    luaL_newlib(L, device_api);
    lua_setglobal(L, "device");
}

/* uart.* — raw transport for not-yet-drivered sensors. Never stores. */
static void lua_register_uart_module(lua_State *L)
{
    static const luaL_Reg uart_api[] = {
        {"query",  l_uart_query},          /* ASCII line query */
        {"status", l_device_uart_status},  /* per-channel connection state */
        {NULL, NULL},
    };

    luaL_newlib(L, uart_api);
    lua_setglobal(L, "uart");
}

static void lua_register_db_module(lua_State *L)
{
    static const luaL_Reg db_api[] = {
        {"store_event", l_db_store_event},
        {"next_id",     l_db_next_id},
        {NULL, NULL},
    };

    luaL_newlib(L, db_api);
    lua_setglobal(L, "db");
}

/* ambit.run(channel, segments, opts) — run an AMBIT fluorescence trace over the
 * binary AMBYTE protocol (cmd 21): the ambit runs the whole trace, then streams
 * each result array back over the FSM handshake as raw uint32 LE (no ASCII, no
 * cJSON). cmd_ambit_run collects them into resp.arrays[] by index; we stream
 * every array into one JSON payload keyed by tag (ENV decoded to leaf-temp degC).
 *
 *   segments : array of { pulses=, freq=, actinic= }  (positional [1],[2],[3] ok)
 *   opts     : { persist=false, store=true, timeout_ms=30000, interrupt=false,
 *                metadata={protocol=..., protocol_id=...} }
 *
 * Returns { points=<fluor len>, stored=K, leaf_temp=degC, arrays=N }. The per-
 * point arrays are persisted to the event log (the point of the run), not
 * materialised back into Lua, to keep memory bounded for large runs. store=true
 * persists one canonical ambit.trace/3 object; no derived fluo ratio is stored. */
/* Caller-owned per-channel state retained between Lua's trigger and fetch. */
static ambit_trace_pending_t s_ambit_pending[UART_SENSOR_NUM_CHANNELS];

/* Only these two user metadata leaves survive the normative v3 contract.
 * Capture them directly while the Lua table exists; building, printing and
 * reparsing a 2-KiB generic JSON object added a failure path without preserving
 * any other field. Overlong optional attribution is omitted whole (never
 * truncated or used to discard the measurement). */
static void ambit_capture_protocol_ref(lua_State *L, int opts_idx,
                                       ambit_protocol_ref_t *ref)
{
    memset(ref, 0, sizeof *ref);
    if (!lua_istable(L, opts_idx)) return;
    lua_getfield(L, opts_idx, "metadata");
    if (lua_istable(L, -1)) {
        const int metadata_idx = lua_gettop(L);
        const char *keys[] = {"protocol", "protocol_id"};
        char *dst[] = {ref->protocol, ref->protocol_id};
        for (size_t i = 0; i < 2U; ++i) {
            lua_getfield(L, -1, keys[i]);
            size_t len = 0;
            const char *value = lua_type(L, -1) == LUA_TSTRING
                ? lua_tolstring(L, -1, &len) : NULL;
            if (value != NULL && len < AMBIT_PROTOCOL_FIELD_CAP) {
                memcpy(dst[i], value, len + 1U);
            } else if (value != NULL) {
                ESP_LOGW(LUA_RUNNER_TAG, "opts.metadata.%s omitted: exceeds %uB",
                         keys[i], (unsigned)(AMBIT_PROTOCOL_FIELD_CAP - 1U));
            } else if (!lua_isnil(L, -1)) {
                ESP_LOGW(LUA_RUNNER_TAG,
                         "opts.metadata.%s ignored: value must be a string", keys[i]);
            }
            lua_pop(L, 1);
        }
        unsigned ignored_count = 0;
        char first_ignored[33] = {0};
        lua_pushnil(L);
        while (lua_next(L, metadata_idx) != 0) {
            size_t key_len = 0;
            const char *key = lua_type(L, -2) == LUA_TSTRING
                ? lua_tolstring(L, -2, &key_len) : NULL;
            const bool supported = key != NULL &&
                (strcmp(key, "protocol") == 0 || strcmp(key, "protocol_id") == 0);
            if (!supported) {
                if (ignored_count != UINT_MAX) ignored_count++;
                if (first_ignored[0] == '\0') {
                    if (key == NULL) {
                        strcpy(first_ignored, "<non-string>");
                    } else {
                        const size_t copy = key_len < sizeof first_ignored - 1U
                            ? key_len : sizeof first_ignored - 1U;
                        memcpy(first_ignored, key, copy);
                        first_ignored[copy] = '\0';
                    }
                }
            }
            lua_pop(L, 1); /* keep key for lua_next */
        }
        if (ignored_count != 0U) {
            ESP_LOGW(LUA_RUNNER_TAG,
                     "opts.metadata ignored %u unsupported key(s), first='%s'; "
                     "only protocol/protocol_id are persisted",
                     ignored_count, first_ignored);
        }
    } else if (!lua_isnil(L, -1)) {
        ESP_LOGW(LUA_RUNNER_TAG, "opts.metadata ignored: value must be a table");
    }
    lua_pop(L, 1);
}

/* Convert the Lua protocol table into the typed, Lua-free trace model. The Lua
 * adapter owns shape/range errors; wire encoding and calibration live in
 * ambit_trace. */
static void ambit_read_segments(lua_State *L, int seg_idx, int nseg,
                                ambit_trace_segment_t *segments)
{
    for (int i = 1; i <= nseg; ++i) {
        lua_rawgeti(L, seg_idx, i);
        if (!lua_istable(L, -1)) luaL_error(L, "segment %d must be a table", i);

        lua_getfield(L, -1, "pulses");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, -1, 1); }
        const lua_Integer pulses = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "freq");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, -1, 2); }
        const lua_Integer freq = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "actinic");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, -1, 3); }
        const lua_Integer actinic = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_pop(L, 1); /* segment table */

        if (pulses < 1 || pulses > UINT16_MAX)
            luaL_error(L, "segment %d: pulses must be 1-65535", i);
        if (freq < 1 || freq > UINT16_MAX)
            luaL_error(L, "segment %d: freq must be 1-65535", i);
        segments[i - 1] = (ambit_trace_segment_t) {
            /* Byte-identical defaults from the former Lua-owned encoder. */
            .type = 2,        /* no IR (notebook default) */
            .far_red = false, /* far-red off */
            .pulses = (uint16_t)pulses,
            .freq = (uint16_t)freq,
            /* The historical binding accepted any Lua integer and mapped values
             * outside the trace model's int16 range to off, not a wrapped DAC. */
            .actinic = actinic >= INT16_MIN && actinic <= INT16_MAX
                ? (int16_t)actinic : 0,
            .subsampling = 1, /* every point */
        };
    }
}

static int ambit_push_result(lua_State *L, esp_err_t err,
                             const ambit_trace_result_t *result)
{
    if (err == ESP_ERR_INVALID_RESPONSE)
        return lua_push_nil_reason(L, "ambit run returned no arrays");
    if (err != ESP_OK)
        return lua_push_nil_reason(L, esp_err_to_name(err));

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)result->points); lua_setfield(L, -2, "points");
    lua_pushinteger(L, result->measure_id >= 0 ? 1 : 0); lua_setfield(L, -2, "stored");
    lua_pushnumber(L, result->leaf_temp); lua_setfield(L, -2, "leaf_temp");
    lua_pushinteger(L, result->array_count); lua_setfield(L, -2, "arrays");
    return 1;
}

static int l_ambit_run(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    if (ch >= UART_SENSOR_NUM_CHANNELS)
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    int nseg = (int)luaL_len(L, 2);
    if (nseg <= 0 || nseg > 16) {
        return luaL_error(L, "segments: 1-16 lines required (got %d)", nseg);
    }

    /* Defaults when opts is omitted (ambit.run(ch, trace)): store + 30 s. */
    uint8_t  persist        = 0;
    bool     store          = true;
    uint32_t timeout_ms     = 30000;
    bool     allow_interrupt = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "persist");
        if (!lua_isnil(L, -1)) persist = lua_toboolean(L, -1) ? 1 : 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "store");
        if (!lua_isnil(L, -1)) store = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "timeout_ms");
        if (lua_isnumber(L, -1)) timeout_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "interrupt");
        if (!lua_isnil(L, -1)) allow_interrupt = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    ambit_protocol_ref_t protocol_ref;
    ambit_capture_protocol_ref(L, 3, &protocol_ref);
    ambit_trace_segment_t trace[PAYLOAD_V3_MAX_SEGMENTS];
    ambit_read_segments(L, 2, nseg, trace);
    uint8_t *run_arr = ambit_trace_build_run_arr(trace, (size_t)nseg, ch);
    if (run_arr == NULL) return luaL_error(L, "out of memory");
    payload_v3_segment_t segments[PAYLOAD_V3_MAX_SEGMENTS];
    ambit_decode_segments(run_arr, (size_t)nseg, segments);

    /* Reconstruct the full ASCII command (with its segment args) for cmd_raw. */
    char cmd_ascii[AMBIT_CMD_ASCII_CAP];
    ambit_build_cmd_ascii(cmd_ascii, sizeof cmd_ascii, run_arr, nseg, persist);

    int64_t start_ms = lua_now_ms();

    /* Binary AMBYTE run (cmd 21): the ambit streams each result array back over
     * the FSM handshake as raw uint32 LE — no ASCII text buffer, no cJSON tree.
     * Arrays land in resp.arrays[] keyed by index (see ambit fw pam_send_results). */
    uart_sensor_response_t resp;
    cmd_result_t res = cmd_ambit_run(ch, run_arr, (uint8_t)nseg, persist,
                                     allow_interrupt, &resp, timeout_ms);
    free(run_arr);

    int64_t end_ms = lua_now_ms();

    if (res.status != ESP_OK) {
        uart_sensor_response_free(&resp);
        return lua_push_nil_reason(L, res.message);
    }
    ambit_trace_result_t result;
    const esp_err_t err = ambit_trace_decode_store(
        &resp, ch, store, start_ms, end_ms, &protocol_ref, cmd_ascii, segments,
        (size_t)nseg, &result);
    return ambit_push_result(L, err, &result);
}

/* ambit.trigger(ch, segments [, opts]) — start a retained (async) run on `ch`
 * and return true once the ambit acks (cmd 22). The run executes on the ambit
 * to completion; collect it later with ambit.poll + ambit.fetch. Stashes the
 * run's metadata + start time per channel for the eventual store.
 *   opts : { persist=false, interrupt=false, timeout_ms=3000,
 *            metadata={protocol=..., protocol_id=...} } */
static int l_ambit_trigger(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    if (ch >= UART_SENSOR_NUM_CHANNELS)
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int nseg = (int)luaL_len(L, 2);
    if (nseg <= 0 || nseg > 16)
        return luaL_error(L, "segments: 1-16 lines required (got %d)", nseg);

    ambit_trace_options_t opts = {
        .timeout_ms = 3000, /* covers wake + ack only */
    };
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "persist");
        if (!lua_isnil(L, -1)) opts.persist = lua_toboolean(L, -1) ? 1 : 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "interrupt");
        if (!lua_isnil(L, -1)) opts.allow_interrupt = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "timeout_ms");
        if (lua_isnumber(L, -1)) opts.timeout_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    ambit_capture_protocol_ref(L, 3, &opts.protocol_ref);
    ambit_trace_segment_t trace[PAYLOAD_V3_MAX_SEGMENTS];
    ambit_read_segments(L, 2, nseg, trace);
    cmd_result_t res = ambit_trace_trigger(ch, trace, (size_t)nseg, &opts,
                                           &s_ambit_pending[ch]);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* ambit.poll(ch [, timeout_ms]) -> "idle" | "done" | "error" | "busy".
 * "busy" = the ambit didn't answer (still measuring, or bus contended) — the
 * caller keeps waiting. Only "done"/"error" are terminal. Default timeout is
 * short so a measuring sensor fails fast instead of being sprayed with wakes. */
static int l_ambit_poll(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    if (ch >= UART_SENSOR_NUM_CHANNELS)
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    uint32_t timeout_ms = (uint32_t)luaL_optinteger(L, 2, 400);

    uint8_t st = 0xFF;
    cmd_result_t res = cmd_ambit_poll(ch, &st, timeout_ms);
    const char *s;
    if (res.status != ESP_OK)         s = "busy";
    else if (st == AMBIT_ASYNC_DONE)  s = "done";
    else if (st == AMBIT_ASYNC_ERROR) s = "error";
    else                              s = "idle";
    lua_pushstring(L, s);
    return 1;
}

/* ambit.fetch(ch [, opts]) -> result table or nil,reason. Streams the retained
 * arrays (cmd 24) and stores one event (store defaults true), using the metadata
 * + start time stashed by ambit.trigger so the event matches ambit.run's.
 *   opts : { store=true, timeout_ms=30000 } */
static int l_ambit_fetch(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    if (ch >= UART_SENSOR_NUM_CHANNELS)
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);

    bool     store      = true;
    uint32_t timeout_ms = 30000;   /* must cover the array stream (scale with size) */
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "store");
        if (!lua_isnil(L, -1)) store = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout_ms");
        if (lua_isnumber(L, -1)) timeout_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    ambit_trace_result_t result;
    const cmd_result_t res = ambit_trace_fetch(
        ch, &s_ambit_pending[ch], store, timeout_ms, &result);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    return ambit_push_result(L, ESP_OK, &result);
}

/* ── ambit.* bindings ────────────────────────────────────────────────
 * The one home for everything AMBIT (protocol-driver namespacing). All
 * functions take the channel as first argument. Measurement commands
 * (spec, leaf_temp, run, fetch) store by default; config/diagnostic
 * commands never store. */
static void lua_register_ambit_module(lua_State *L)
{
    /* Reserve the shared run payload up front while the heap is contiguous. */
    if (ambit_trace_reserve() != ESP_OK) {
        ESP_LOGW(LUA_RUNNER_TAG,
                 "ambit.run payload reserve (%uB) failed; will retry per-run",
                 (unsigned)AMBIT_RUN_BUFFER_CAP);
    }

    static const luaL_Reg ambit_api[] = {
        /* presence / identity */
        {"ping",          l_device_uart_ping},        /* 0xAA wake → 0x80 ack */
        {"get_info",      l_device_ambit_get_info},
        /* measurements (fused store, {store=false} to opt out) */
        {"spec",          l_ambit_spec},              /* cmd_raw "get_par"  */
        {"leaf_temp",     l_ambit_leaf_temp},         /* cmd_raw "get_temp" */
        {"run",           l_ambit_run},               /* cmd_raw "arrun"    */
        {"trigger",       l_ambit_trigger},   /* parallel protocol: start retained run */
        {"poll",          l_ambit_poll},      /*   "    "    "    : query async state  */
        {"fetch",         l_ambit_fetch},     /*   "    "    "    : collect + store    */
        /* diagnostics (never store) */
        {"leaf_temp_raw", l_device_ambit_get_temp_raw},
        {"query",         l_ambit_uart_query},        /* raw 8-byte binary frames */
        {"run_raw",       l_device_ambit_run},        /* legacy raw run_arr run   */
        {"run_mpf",       l_device_ambit_run_mpf},
        /* configuration / actions */
        {"set_gains",     l_device_ambit_set_gains},
        {"set_currents",  l_device_ambit_set_currents},
        {"config_detector", l_device_ambit_config_detector},
        {"blink",         l_device_ambit_blink},
        {"calibrate_baseline", l_device_ambit_calibrate_baseline},
        {"actinic",       l_device_ambit_actinic},
        {"set_metadata",  l_device_ambit_set_metadata},
        {NULL, NULL},
    };

    luaL_newlib(L, ambit_api);
    lua_setglobal(L, "ambit");
}

/* ── sync.* bindings (RTC-based scheduling; see components/time_sync) ──── */

/* current RTC time as local Unix seconds; 0 if the RTC is unavailable */
static int64_t sync_now(void)
{
    time_t t = 0;
    cmd_read_rtc(&t);                       /* UTC — the RTC holds UTC by design */
    /* time_sync reasons in LOCAL wall time; hand it a localized `now` (and, as a
     * side effect, refresh its offset so the sunrise/sunset path stays on the
     * same frame). DST-correct via the applied IANA timezone; see
     * components/timezone. */
    return timezone_localize((int64_t)t);
}

/* sync.until_interval(period_s [, phase_s]) -> seconds */
static int l_sync_until_interval(lua_State *L)
{
    int64_t period = (int64_t)luaL_checkinteger(L, 1);
    int64_t phase  = (int64_t)luaL_optinteger(L, 2, 0);
    int64_t s = time_sync_until_interval(sync_now(), period, phase);
    if (s < 0) return lua_push_nil_reason(L, "period must be > 0");
    lua_pushinteger(L, (lua_Integer)s);
    return 1;
}

/* sync.until_clock(hour, min [, sec]) -> seconds */
static int l_sync_until_clock(lua_State *L)
{
    int hour = (int)luaL_checkinteger(L, 1);
    int min  = (int)luaL_checkinteger(L, 2);
    int sec  = (int)luaL_optinteger(L, 3, 0);
    lua_pushinteger(L, (lua_Integer)time_sync_until_clock(sync_now(), hour, min, sec));
    return 1;
}

/* days table -> mask (accepts "mon"/"tue"… or 1=Sun..7=Sat); 0 on bad input */
static uint8_t sync_days_mask(lua_State *L, int idx)
{
    uint8_t mask = 0;
    if (!lua_istable(L, idx)) return 0;
    lua_Integer n = luaL_len(L, idx);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, (int)i);
        if (lua_isstring(L, -1)) {
            int b = time_sync_day_bit(lua_tostring(L, -1));
            if (b >= 0) mask |= (uint8_t)(1u << b);
        } else if (lua_isinteger(L, -1)) {
            lua_Integer d = lua_tointeger(L, -1);     /* 1=Sun..7=Sat */
            if (d >= 1 && d <= 7) mask |= (uint8_t)(1u << (d - 1));
        }
        lua_pop(L, 1);
    }
    return mask;
}

/* sync.until_weekly(days, hour, min) -> seconds */
static int l_sync_until_weekly(lua_State *L)
{
    uint8_t mask = sync_days_mask(L, 1);
    int hour = (int)luaL_checkinteger(L, 2);
    int min  = (int)luaL_checkinteger(L, 3);
    int64_t s = time_sync_until_weekly(sync_now(), mask, hour, min);
    if (s < 0) return lua_push_nil_reason(L, "no valid weekdays");
    lua_pushinteger(L, (lua_Integer)s);
    return 1;
}

/* sync.until_sun("sunrise"|"sunset", offset_s) -> seconds */
static int l_sync_until_sun(lua_State *L)
{
    const char *ev = luaL_checkstring(L, 1);
    int64_t offset = (int64_t)luaL_optinteger(L, 2, 0);
    int event = (strcasecmp(ev, "sunset") == 0) ? TIME_SYNC_SUNSET : TIME_SYNC_SUNRISE;
    int64_t s = time_sync_until_sun(sync_now(), event, offset);
    if (s < 0) return lua_push_nil_reason(L, "no sun event (polar?)");
    lua_pushinteger(L, (lua_Integer)s);
    return 1;
}

/* sync.sun_today() -> sunrise_str, sunset_str  ("HH:MM" RTC, or "--:--") */
static int l_sync_sun_today(lua_State *L)
{
    int64_t now = sync_now();
    char sr[8], ss[8];
    int64_t u;
    int h, m;
    if (time_sync_sun_on_date(now, TIME_SYNC_SUNRISE, &u) == ESP_OK) {
        time_sync_localtime(u, NULL, NULL, NULL, &h, &m, NULL, NULL);
        snprintf(sr, sizeof(sr), "%02d:%02d", h, m);
    } else { snprintf(sr, sizeof(sr), "--:--"); }
    if (time_sync_sun_on_date(now, TIME_SYNC_SUNSET, &u) == ESP_OK) {
        time_sync_localtime(u, NULL, NULL, NULL, &h, &m, NULL, NULL);
        snprintf(ss, sizeof(ss), "%02d:%02d", h, m);
    } else { snprintf(ss, sizeof(ss), "--:--"); }
    lua_pushstring(L, sr);
    lua_pushstring(L, ss);
    return 2;
}

/* sync.set_location(lat, lon [, tz_hours]) */
static int l_sync_set_location(lua_State *L)
{
    double lat = luaL_checknumber(L, 1);
    double lon = luaL_checknumber(L, 2);
    int tz_cur;
    time_sync_get_location(NULL, NULL, &tz_cur);
    int tz = (int)luaL_optinteger(L, 3, tz_cur);
    time_sync_set_location(lat, lon, tz);
    return 0;
}

/* sync.location() -> {lat=, lon=, tz=} */
static int l_sync_location(lua_State *L)
{
    double lat, lon; int tz;
    time_sync_get_location(&lat, &lon, &tz);
    lua_newtable(L);
    lua_pushnumber(L, lat); lua_setfield(L, -2, "lat");
    lua_pushnumber(L, lon); lua_setfield(L, -2, "lon");
    lua_pushinteger(L, tz); lua_setfield(L, -2, "tz");
    return 1;
}

/* sync.wait(seconds) — block until elapsed, polling the hardware RTC in 30 s
 * chunks (drift-free + interruptible by lua_runner_stop). */
static int l_sync_wait(lua_State *L)
{
    lua_Integer seconds = luaL_checkinteger(L, 1);
    if (seconds < 0) return luaL_error(L, "seconds must be >= 0");
    int64_t target = sync_now() + (int64_t)seconds;
    while (true) {
        if (s_should_stop) {
            return luaL_error(L, "lua_runner: stop signaled during sync.wait");
        }
        int64_t remaining = target - sync_now();
        if (remaining <= 0) break;
        uint32_t step = (remaining < 30) ? (uint32_t)remaining : 30;
        cmd_sleep_ms(step * 1000);
    }
    return 0;
}

/* sync.is_daytime() -> true|false (currently between sunrise and sunset) */
static int l_sync_is_daytime(lua_State *L)
{
    lua_pushboolean(L, time_sync_is_daytime(sync_now()) ? 1 : 0);
    return 1;
}

static void lua_register_sync_module(lua_State *L)
{
    static const luaL_Reg sync_api[] = {
        {"until_interval", l_sync_until_interval},
        {"until_clock",    l_sync_until_clock},
        {"until_weekly",   l_sync_until_weekly},
        {"until_sun",      l_sync_until_sun},
        {"sun_today",      l_sync_sun_today},
        {"is_daytime",     l_sync_is_daytime},
        {"set_location",   l_sync_set_location},
        {"location",       l_sync_location},
        {"wait",           l_sync_wait},
        {NULL, NULL},
    };

    luaL_newlib(L, sync_api);
    lua_setglobal(L, "sync");
}

/* ── task ────────────────────────────────────────────────────────────── */

/* Open the standard libraries + the full ambyte module set (device/uart/db/
 * ambit/sync globals) into `L`. Shared by the main-script task and the
 * ephemeral lua_runner_exec states — every register function is self-contained
 * per lua_State. `sched` is NOT part of the env: the task loads it separately
 * (exec snippets have no scheduler loop to run). */
static void lua_open_ambyte_env(lua_State *L)
{
    luaL_openlibs(L);
    lua_register_device_module(L);
    lua_register_uart_module(L);
    lua_register_db_module(L);
    lua_register_ambit_module(L);
    lua_register_sync_module(L);
}

static void log_lua_error(lua_State *L, const char *phase)
{
    const char *msg = lua_tostring(L, -1);
    ESP_LOGE(LUA_RUNNER_TAG, "%s: %s", phase, msg ? msg : "unknown Lua error");
    lua_pop(L, 1);
}

/* Debug hook fired every N Lua bytecode instructions. Raises a Lua error if
 * lua_runner_stop() has been signaled — unwinds the running script cleanly. */
static void lua_stop_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (s_should_stop) {
        luaL_error(L, "lua_runner: stop signaled");
    }
    /* Service a heap-GC request from the MQTT publisher (safe here: we hold L in
     * the Lua task). A full collect coalesces the shared heap so a large publish
     * can find its contiguous outbox + TLS buffers. Rare (per large arrun trace). */
    if (s_gc_request) {
        s_gc_request = false;
        lua_gc(L, LUA_GCCOLLECT, 0);
    }
}

/* Ask the Lua task to run a full GC at its next hook tick (~every 1000 bytecode
 * instructions). Safe to call from any task — it only sets a flag; the collect
 * itself runs in the Lua task. No-op effect until the script executes bytecode
 * (a script blocked in a long C call services it once it resumes). */
void lua_runner_request_gc(void)
{
    s_gc_request = true;
}

static void lua_runner_task(void *arg)
{
    (void)arg;

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        ESP_LOGE(LUA_RUNNER_TAG, "Failed to create Lua state");
        goto done;
    }

    lua_open_ambyte_env(L);

    /* Define the bundled `sched` global (scheduler library) before the user
     * script. The chunk returns its module table, which we install as `sched`. */
    {
        if (luaL_loadbuffer(L, sched_lua_start, sched_lua_size, "=sched.lua") != LUA_OK ||
            lua_pcall(L, 0, 1, 0) != LUA_OK) {
            log_lua_error(L, "failed to load bundled sched.lua");
            lua_close(L);
            goto done;
        }
        lua_setglobal(L, "sched");
    }

    /* Fire the stop-check every 1000 Lua bytecode instructions — cheap and
     * gives prompt unwinding without measurable interpreter slowdown. */
    lua_sethook(L, lua_stop_hook, LUA_MASKCOUNT, 1000);

    if (luaL_loadfile(L, LUA_SCRIPT_PATH) != LUA_OK) {
        log_lua_error(L, "failed to load " LUA_SCRIPT_PATH);
        lua_close(L);
        goto done;
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        if (s_should_stop) {
            /* Deliberate stop (e.g. SD card removed) — the script was unwound on
             * purpose via the stop hook / interruptible sleep. Not a failure. */
            const char *msg = lua_tostring(L, -1);
            ESP_LOGI(LUA_RUNNER_TAG, "script stopped: %s", msg ? msg : "stop requested");
            lua_pop(L, 1);
        } else {
            log_lua_error(L, "lua_pcall failed");
        }
    }

    ESP_LOGI(LUA_RUNNER_TAG, "Script finished, stack high water: %lu",
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));

    lua_close(L);

done:
    s_lua_task_handle = NULL;
    if (s_done_sem != NULL) {
        xSemaphoreGive(s_done_sem);
    }
    vTaskDelete(NULL);
}

/* Copy an external script (normally /sdcard/main.lua — the manual offline
 * recovery path: no internet, tech writes a card on a laptop, inserts it) into
 * the canonical internal home. Staged copy + atomic rename, previous script kept
 * as .bak — the same swap discipline script_update uses, so a reset mid-import
 * can never leave the device script-less. No syntax check here: a broken import
 * fails loudly at the next start and the .bak remains for manual recovery. */
esp_err_t lua_runner_import_script(const char *src_path)
{
    if (src_path == NULL) return ESP_ERR_INVALID_ARG;
    FILE *in = fopen(src_path, "rb");
    if (in == NULL) return ESP_ERR_NOT_FOUND;

    FILE *out = fopen(LUA_SCRIPT_PATH_NEW, "wb");
    if (out == NULL) { fclose(in); return ESP_FAIL; }

    char buf[512];
    size_t n, total = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
        total += n;
    }
    if (ok && ferror(in)) ok = false;
    if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0)) ok = false;
    fclose(in);
    fclose(out);
    if (!ok || total == 0) {
        remove(LUA_SCRIPT_PATH_NEW);
        return ESP_FAIL;
    }

    remove(LUA_SCRIPT_PATH_BAK);
    rename(LUA_SCRIPT_PATH, LUA_SCRIPT_PATH_BAK);   /* ENOENT on first install: fine */
    if (rename(LUA_SCRIPT_PATH_NEW, LUA_SCRIPT_PATH) != 0) {
        remove(LUA_SCRIPT_PATH_NEW);
        return ESP_FAIL;
    }
    ESP_LOGW(LUA_RUNNER_TAG, "imported %s -> %s (%u bytes; previous kept as .bak)",
             src_path, LUA_SCRIPT_PATH, (unsigned)total);
    return ESP_OK;
}

bool lua_runner_script_present(void)
{
    struct stat st;
    return stat(LUA_SCRIPT_PATH, &st) == 0 && st.st_size > 0;
}

esp_err_t lua_runner_start(void)
{
    if (s_lua_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_done_sem == NULL) {
        s_done_sem = xSemaphoreCreateBinaryStatic(&s_done_sem_storage);
        if (s_done_sem == NULL) return ESP_ERR_NO_MEM;
    } else {
        /* Drain any leftover give from a previous run so stop() waits on the
         * NEW task's exit, not a stale signal. */
        (void)xSemaphoreTake(s_done_sem, 0);
    }
    s_should_stop = false;

    BaseType_t created = xTaskCreatePinnedToCore(
        lua_runner_task,
        LUA_RUNNER_TASK_NAME,
        LUA_RUNNER_TASK_STACK,
        NULL,
        LUA_RUNNER_TASK_PRIORITY,
        &s_lua_task_handle,
        LUA_RUNNER_TASK_CORE
    );
    if (created != pdPASS) {
        s_lua_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool lua_runner_is_running(void)
{
    return s_lua_task_handle != NULL;
}

esp_err_t lua_runner_stop(uint32_t wait_ms)
{
    if (s_lua_task_handle == NULL) {
        return ESP_OK;     /* nothing running */
    }
    s_should_stop = true;
    if (s_done_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The Lua VM unwinds at the next bytecode (or sleep chunk). If the task is
     * stuck in a long C-blocking call (e.g. a 30 s UART read), we time out;
     * it'll still exit on its own once that call returns. */
    if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(LUA_RUNNER_TAG, "stop: task still busy after %u ms — will exit later",
                 (unsigned)wait_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ── on-demand snippet execution (CLI `lua exec` / MQTT lua_exec) ─────────── */

/* Registry keys for the exec deadline — per-state, so concurrent execs (CLI +
 * MQTT worker) and the main script don't share timer state. */
#define LUA_EXEC_KEY_START   "ambyte_exec_start_ms"
#define LUA_EXEC_KEY_BUDGET  "ambyte_exec_budget_ms"

/* Count hook for exec states: abort once the wall-clock budget is spent.
 * Wrap-safe (uint32 subtraction); esp_log_timestamp wraps after ~49 days. */
static void lua_exec_deadline_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_EXEC_KEY_START);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_EXEC_KEY_BUDGET);
    uint32_t start  = (uint32_t)lua_tointeger(L, -2);
    uint32_t budget = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 2);
    if (budget > 0 && (uint32_t)(esp_log_timestamp() - start) > budget) {
        luaL_error(L, "lua_runner: exec timeout (%u ms)", (unsigned)budget);
    }
}

esp_err_t lua_runner_exec(const char *code, uint32_t timeout_ms,
                          char *result, size_t result_cap)
{
    if (result != NULL && result_cap > 0) result[0] = '\0';
    if (code == NULL || code[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* Fresh, ephemeral state with the full ambyte env (no sched, no stop hook):
     * runs ALONGSIDE a running main.lua — same powers, separate VM. Shared
     * resources (UART transactions, the ambit payload buffer) are serialized at
     * the C layer, so the two can't corrupt each other, only wait. */
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        if (result != NULL && result_cap > 0) {
            snprintf(result, result_cap, "out of memory creating Lua state");
        }
        return ESP_ERR_NO_MEM;
    }
    lua_open_ambyte_env(L);

    if (timeout_ms > 0) {
        lua_pushinteger(L, (lua_Integer)esp_log_timestamp());
        lua_setfield(L, LUA_REGISTRYINDEX, LUA_EXEC_KEY_START);
        lua_pushinteger(L, (lua_Integer)timeout_ms);
        lua_setfield(L, LUA_REGISTRYINDEX, LUA_EXEC_KEY_BUDGET);
        lua_sethook(L, lua_exec_deadline_hook, LUA_MASKCOUNT, 1000);
    }

    esp_err_t ret;
    const int top0 = lua_gettop(L);
    if (luaL_loadstring(L, code) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        ESP_LOGW(LUA_RUNNER_TAG, "exec: syntax error: %s", msg ? msg : "?");
        if (result != NULL && result_cap > 0) {
            snprintf(result, result_cap, "%s", msg ? msg : "syntax error");
        }
        ret = ESP_ERR_INVALID_ARG;
    } else if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        ESP_LOGW(LUA_RUNNER_TAG, "exec: %s", msg ? msg : "runtime error");
        if (result != NULL && result_cap > 0) {
            snprintf(result, result_cap, "%s", msg ? msg : "runtime error");
        }
        ret = ESP_FAIL;
    } else {
        if (lua_gettop(L) > top0 && result != NULL && result_cap > 0) {
            /* First returned value, via tostring semantics (honors __tostring). */
            const char *s = luaL_tolstring(L, top0 + 1, NULL);
            snprintf(result, result_cap, "%s", s ? s : "");
            lua_pop(L, 1);
        }
        ret = ESP_OK;
    }

    lua_close(L);   /* frees the state's transient allocations in full */
    return ret;
}

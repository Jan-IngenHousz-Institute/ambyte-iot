#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "device_commands.h"
#include "ambit_protocol.h"   /* AMBIT_ASYNC_* run-state constants for ambit.poll */
#include "time_sync.h"
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
#define LUA_QUERY_MAX_RECORDS 64
#define LUA_SCRIPT_PATH "/sdcard/main.lua"

/* Bundled scheduler library (components/lua_runner/sched.lua), generated into a
 * C byte array at configure time (see CMakeLists) and loaded before the user
 * script to define the `sched` global. Provides sched_lua_start[] + sched_lua_size. */
#include "sched_lua_embed.h"

static TaskHandle_t s_lua_task_handle = NULL;

/* Stop-flag inspected by the Lua debug hook + the interruptible sleep. Set by
 * lua_runner_stop(); the script unwinds via luaL_error and the task exits. */
static volatile bool s_should_stop = false;

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

static int l_device_set_rgb(lua_State *L)
{
    lua_Integer r = luaL_checkinteger(L, 1);
    lua_Integer g = luaL_checkinteger(L, 2);
    lua_Integer b = luaL_checkinteger(L, 3);

    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        return luaL_error(L, "RGB values must be in [0, 255]");
    }

    cmd_result_t res = cmd_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
    if (res.status != ESP_OK) {
        return luaL_error(L, "%s", res.message);
    }
    return 0;
}

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

static int l_device_read_env(lua_State *L)
{
    float temp = 0, hum = 0, pres = 0;
    cmd_result_t res = cmd_read_env(&temp, &hum, &pres);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)temp);
    lua_setfield(L, -2, "temperature_c");
    lua_pushnumber(L, (lua_Number)hum);
    lua_setfield(L, -2, "humidity_pct");
    lua_pushnumber(L, (lua_Number)pres);
    lua_setfield(L, -2, "pressure_pa");
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

/* device.record_env() — read BME280 + persist T/H/P as three rows sharing one
 * measure_id. Returns that measure_id (int) or nil,err. */
static int l_device_record_env(lua_State *L)
{
    int64_t mid = 0;
    cmd_result_t res = cmd_record_env(&mid);
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushinteger(L, (lua_Integer)mid);
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

/* device.measurement_window(on) — assert/release the measurement-activity gate
 * around a whole measurement cycle so the sync_runner doesn't publish mid-cycle.
 * begin/end are reference-counted; pair every on with exactly one off. */
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
 * to be stored via db.store_event{ sensor="status", data=... } so the sole
 * publisher (sync_runner) forwards it under the power gate. */
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

/* ── device.uart_* bindings ──────────────────────────────────────────── */

/* device.uart_ping(channel) → boolean */
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

/* device.uart_status() → { [0]="connected", [1]="disconnected", ... } */
static int l_device_uart_status(lua_State *L)
{
    cmd_result_t res = cmd_uart_status();
    if (res.status != ESP_OK) {
        return lua_push_nil_reason(L, res.message);
    }
    lua_pushstring(L, res.message);
    return 1;
}

/* device.uart_query(ch, cmd, timeout_ms, opts) — ASCII line-oriented query.
 *
 *   ch          : channel index 0..3
 *   cmd         : ASCII command string (terminator appended by the runtime)
 *   timeout_ms  : total budget for send + read in ms
 *   opts        : optional table:
 *                   lineterminator = "\n"           -- default
 *                   save           = true           -- default; persist row
 *
 * Returns the response string (terminator stripped) or "" on timeout. On a
 * hard error returns nil, err. Echo handling: if the first line read back
 * equals `cmd` verbatim, it is dropped and the next line is returned. */
static int l_device_uart_query(lua_State *L)
{
    lua_Integer ch = luaL_checkinteger(L, 1);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    }
    const char *cmd        = luaL_checkstring(L, 2);
    lua_Integer timeout_ms = luaL_checkinteger(L, 3);

    const char *terminator = "\n";
    bool save = true;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "lineterminator");
        if (lua_isstring(L, -1)) {
            terminator = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "save");
        if (lua_isboolean(L, -1)) {
            save = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    char   response[256];
    size_t resp_len = 0;
    cmd_result_t res = cmd_uart_text_query((uint8_t)ch, cmd, terminator,
                                           (uint32_t)timeout_ms, save,
                                           response, sizeof(response), &resp_len);

    /* Hard error (anything other than success or pure timeout). */
    if (res.status != ESP_OK && res.status != ESP_ERR_TIMEOUT) {
        return lua_push_nil_reason(L, res.message);
    }

    /* On both success and timeout, return the response (possibly empty). */
    lua_pushlstring(L, response, resp_len);
    return 1;
}

/* ambit.uart_query(channel, cmd_bytes, extra_bytes_or_nil, expect_raw, timeout_ms)
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

/* ── device.ambit_* bindings ─────────────────────────────────────────── */

/* device.ambit_set_gains(ch, fluo, fluoref, ir, irref, sun, leaf) → true or nil,err */
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

/* device.ambit_set_currents(ch, i620, i720, ir) → true or nil,err */
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

/* device.ambit_config_detector(ch) → true or nil,err */
static int l_device_ambit_config_detector(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    cmd_result_t res = cmd_ambit_config_detector(ch);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* device.ambit_get_temp(ch) → {leaf=float, chip=float} or nil,err */
static int l_device_ambit_get_temp(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    float leaf = 0, chip = 0;
    cmd_result_t res = cmd_ambit_get_temp(ch, &leaf, &chip);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)leaf);  lua_setfield(L, -2, "leaf");
    lua_pushnumber(L, (lua_Number)chip);  lua_setfield(L, -2, "chip");
    return 1;
}

/* device.ambit_get_spec(ch) → {spec={10 ints}, par=float} or nil,err */
static int l_device_ambit_get_spec(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    uint16_t spec[10] = {0};
    float par = 0;
    cmd_result_t res = cmd_ambit_get_spec(ch, spec, &par);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_newtable(L);
    lua_newtable(L);
    for (int i = 0; i < 10; i++) {
        lua_pushinteger(L, spec[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "spec");
    lua_pushnumber(L, (lua_Number)par);
    lua_setfield(L, -2, "par");
    return 1;
}

/* device.ambit_get_temp_raw(ch) → {leaf,leaf1,chip,raw={4 ints}} or nil,err */
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

/* device.ambit_get_info(ch, type) → string (raw bytes) or nil,err */
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

/* device.ambit_run(ch, run_arr_table, led_persist, allow_interrupt, timeout_ms)
 *   run_arr_table: flat table of integers (length must be multiple of 8)
 *   Returns: response table (same as uart_query FSM mode) or nil,err */
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

/* device.ambit_run_mpf(ch, length, interval, change_act, act, timeout_ms) → table */
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

/* device.ambit_blink(ch, id, intensity) → true or nil,err */
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

/* device.ambit_calibrate_baseline(ch) → true or nil,err */
static int l_device_ambit_calibrate_baseline(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    cmd_result_t res = cmd_ambit_calibrate_baseline(ch);
    if (res.status != ESP_OK) return lua_push_nil_reason(L, res.message);
    lua_pushboolean(L, 1);
    return 1;
}

/* device.ambit_actinic(ch, type, var, var2) → true or nil,err */
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

/* device.ambit_set_metadata(ch, metadata_string) → true or nil,err */
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

/* Take an optional Lua field at index -1 (table on top of stack). Pops it.
 * Writes a non-empty string to `out` (truncated to cap-1). */
static void lua_get_optional_string(lua_State *L, const char *key,
                                    char *out, size_t cap)
{
    lua_getfield(L, -1, key);
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        strncpy(out, s, cap - 1);
        out[cap - 1] = '\0';
    }
    lua_pop(L, 1);
}

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

/* db.store_event{ sensor=, device=, data={...}, metadata=, measure_id=,
 *                 start_ticks=, end_ticks= }
 *
 *   sensor       required string ("BME280", "AMBIT", …)
 *   data         required table → JSON object of quantities (the payload)
 *   device       optional string; "" / nil = onboard sensor
 *   metadata     optional table → JSON object (or omit)
 *   measure_id   optional int; auto-allocated when omitted
 *   start_ticks  optional UTC-ms; defaults to now
 *   end_ticks    optional UTC-ms; defaults to start_ticks
 *
 * Returns measure_id on success, or nil,reason. */
static int l_db_store_event(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* sensor — required */
    char sensor[24] = {0};
    lua_get_optional_string(L, "sensor", sensor, sizeof(sensor));
    if (sensor[0] == '\0') {
        return lua_push_nil_reason(L, "store_event: 'sensor' (string) required");
    }

    /* device — optional ("" / nil = onboard) */
    char device[24] = {0};
    lua_get_optional_string(L, "device", device, sizeof(device));

    /* measure_id — optional; auto-allocate when missing */
    int64_t measure_id = 0;
    lua_getfield(L, 1, "measure_id");
    bool has_id = (lua_isinteger(L, -1) || lua_isnumber(L, -1));
    if (has_id) measure_id = (int64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (!has_id) {
        cmd_result_t idr = cmd_next_measure_id(&measure_id);
        if (idr.status != ESP_OK) return lua_push_nil_reason(L, idr.message);
    }

    /* start_ticks / end_ticks — optional; default to now */
    int64_t now_ms;
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        now_ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
    }
    lua_getfield(L, 1, "start_ticks");
    int64_t start_ms = (lua_isinteger(L, -1) || lua_isnumber(L, -1))
                       ? (int64_t)lua_tointeger(L, -1) : now_ms;
    lua_pop(L, 1);
    lua_getfield(L, 1, "end_ticks");
    int64_t end_ms = (lua_isinteger(L, -1) || lua_isnumber(L, -1))
                     ? (int64_t)lua_tointeger(L, -1) : start_ms;
    lua_pop(L, 1);

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

    /* Schema-v2 shim (Phase 1): the legacy Lua 'sensor' string rides in cmd_raw
     * so its discriminator survives the transition; tag is firmware-fixed.
     * Phase 2 slims the Lua API to { data, metadata, channel }. */
    measurement_event_desc_t d = {
        .measure_id    = measure_id,
        .device        = device,
        .tag           = MEASUREMENT_TAG_MEASUREMENT,
        .cmd_raw       = sensor,
        .start_ms      = start_ms,
        .end_ms        = end_ms,
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

static void lua_register_device_module(lua_State *L)
{
    static const luaL_Reg device_api[] = {
        {"set_rgb",                l_device_set_rgb},
        {"read_rtc",               l_device_read_rtc},
        {"status",                 l_device_status},
        {"read_env",               l_device_read_env},
        {"power",                  l_device_power},
        {"record_env",             l_device_record_env},
        {"sd_ready",               l_device_sd_ready},
        {"sleep_ms",               l_device_sleep_ms},
        {"log",                    l_device_log},
        {"measurement_window",     l_device_measurement_window},
        {"uptime_ms",              l_device_uptime_ms},
        {"status_report",          l_device_status_report},
        {"PWM",                    l_device_pwm},
        /* UART raw */
        {"uart_ping",              l_device_uart_ping},
        {"uart_query",             l_device_uart_query},
        {"uart_status",            l_device_uart_status},
        /* Ambit typed commands */
        {"ambit_set_gains",        l_device_ambit_set_gains},
        {"ambit_set_currents",     l_device_ambit_set_currents},
        {"ambit_config_detector",  l_device_ambit_config_detector},
        {"ambit_get_temp",         l_device_ambit_get_temp},
        {"ambit_get_spec",         l_device_ambit_get_spec},
        {"ambit_get_temp_raw",     l_device_ambit_get_temp_raw},
        {"ambit_get_info",         l_device_ambit_get_info},
        {"ambit_run",              l_device_ambit_run},
        {"ambit_run_mpf",          l_device_ambit_run_mpf},
        {"ambit_blink",            l_device_ambit_blink},
        {"ambit_calibrate_baseline", l_device_ambit_calibrate_baseline},
        {"ambit_actinic",          l_device_ambit_actinic},
        {"ambit_set_metadata",     l_device_ambit_set_metadata},
        {NULL, NULL},
    };

    luaL_newlib(L, device_api);
    lua_setglobal(L, "device");
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

/* Parse "<key><number>" out of an ASCII line (e.g. key "T:" from
 * "T:23.45,F:.."). Returns true and writes *out when found. */
static bool ambit_parse_field(const char *line, const char *key, double *out)
{
    const char *p = strstr(line, key);
    if (p == NULL) return false;
    *out = strtod(p + strlen(key), NULL);
    return true;
}

/* Array index → JSON tag, matching the ambit fw AMBYTE send order
 * (PAM.cpp pam_send_results): 0=ENV(leaf temp), 1=fluor, 2=fluoRef, 3=sun,
 * 4=leaf, 5=730, 6=730ref, 7=TIMING. NULL → caller falls back to "arr<idx>". */
static const char *ambit_array_tag(uint8_t idx)
{
    switch (idx) {
        case 0: return "env";      /* ENV — leaf temperature, degC */
        case 1: return "s_fluo";   /* Fluo — fluorescence signal */
        case 2: return "r_fluo";   /* Fluoref — fluorescence reference */
        case 3: return "sun";
        case 4: return "leaf";
        case 5: return "s_730";    /* 730 signal */
        case 6: return "r_730";    /* 730 reference */
        case 7: return "timing";   /* uint32 [tick_begin, tick_end] µs */
        default: return NULL;
    }
}

/* Reserved once (at module register, while the heap is still contiguous) and
 * reused for every run's JSON payload — the run's one big allocation, kept off
 * the fragmenting per-run path. Only the lua_runner task calls ambit.run, so a
 * single shared buffer is safe. Sized for current point counts; larger runs
 * truncate (logged). A uint16/compact-storage v2 will shrink this need. */
#define AMBIT_RUN_PAYLOAD_CAP (8 * 1024)
static char *s_ambit_payload;

/* ambit.run(channel, segments, opts) — run an AMBIT fluorescence trace over the
 * binary AMBYTE protocol (cmd 21): the ambit runs the whole trace, then streams
 * each result array back over the FSM handshake as raw uint32 LE (no ASCII, no
 * cJSON). cmd_ambit_run collects them into resp.arrays[] by index; we stream
 * every array into one JSON payload keyed by tag (ENV decoded to leaf-temp degC).
 *
 *   segments : array of { pulses=, freq=, actinic= }  (positional [1],[2],[3] ok)
 *   opts     : { persist=false, store=true, timeout_ms=30000, interrupt=false }
 *
 * Returns { points=<fluor len>, stored=K, leaf_temp=degC, arrays=N }. The per-
 * point arrays are persisted to the DB (the point of the run), not materialised
 * back into Lua, to keep memory bounded for large runs. store=true persists one
 * event whose payload is {"leaf_temp":[…],"fluor":[…],"fluoRef":[…],…}. */
/* Actinic value → AMBIT LED-current byte, matching the original WRENCH ambyte
 * (protocol.cpp::generate_arr): a negative value (-255..-1) is an exact DAC level
 * (|value|); a positive value (1..9999) is PAR in µmol → byte = par_coef × PAR,
 * floored at 4 and capped at 255; anything else (0 / out of range) = 0 (off).
 * par_coef is the AMBIT's actinic calibration (fallback 0.05 byte/µmol). */
static uint8_t ambit_actinic_to_dac(lua_Integer actinic, float par_coef)
{
    if (actinic < 0 && actinic > -256) return (uint8_t)(-actinic);
    if (actinic > 0 && actinic < 10000) {
        float t = par_coef * (float)actinic;
        if (t < 4.0f)   return 4;
        if (t > 255.0f) return 255;
        return (uint8_t)t;
    }
    return 0;
}

/* Per-channel state stashed by ambit.trigger for the eventual ambit.fetch store:
 * the run metadata (segments JSON) and the measurement start time. */
static char    s_ambit_meta[UART_SENSOR_NUM_CHANNELS][768];
static int64_t s_ambit_start_ms[UART_SENSOR_NUM_CHANNELS];

/* Build a malloc'd run_arr (caller frees) of nseg*8 bytes plus a metadata JSON
 * string, from the Lua segments table at `seg_idx`, for channel `ch` (its actinic
 * calibration sets the PAR→DAC coefficient). Shared by ambit.run and ambit.trigger
 * so both encode the wire identically. luaL_errors (longjmp) on malformed input. */
static uint8_t *ambit_build_run_arr(lua_State *L, int seg_idx, uint8_t ch, int nseg,
                                    char *metadata_json, size_t meta_cap)
{
    uint8_t *run_arr = malloc((size_t)nseg * 8);
    if (!run_arr) { luaL_error(L, "out of memory"); return NULL; }

    /* This AMBIT's PAR→DAC actinic coefficient — lazily fetched once + cached
     * (cmd 33). Fallback 0.05 byte/µmol if the calibration can't be read. */
    ambit_device_info_t info;
    cmd_ambit_device_info(ch, &info);
    float par_coef = (info.valid && info.actinic_coef > 0.0f) ? info.actinic_coef : 0.05f;

    for (int i = 1; i <= nseg; i++) {
        lua_rawgeti(L, seg_idx, i);
        if (!lua_istable(L, -1)) {
            free(run_arr);
            luaL_error(L, "segment %d must be a table", i);
        }

        lua_getfield(L, -1, "pulses");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, -1, 1); }
        lua_Integer pulses = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "freq");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, -1, 2); }
        lua_Integer freq = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "actinic");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, -1, 3); }
        lua_Integer actinic = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_pop(L, 1); /* segment table */

        if (pulses < 1 || pulses > 65535) { free(run_arr); luaL_error(L, "segment %d: pulses must be 1-65535", i); }
        if (freq   < 1 || freq   > 65535) { free(run_arr); luaL_error(L, "segment %d: freq must be 1-65535", i); }
        /* actinic: WRENCH convention — -255..-1 = raw DAC, 1..9999 = PAR (µmol),
         * 0 / out-of-range = off. Converted per the AMBIT's calibration below. */

        uint8_t *line = run_arr + (i - 1) * 8;
        line[0] = 2;                              /* type 2: no IR (notebook default) */
        line[1] = 0;                              /* far-red off */
        line[2] = (uint8_t)((pulses >> 8) & 0xFF);
        line[3] = (uint8_t)(pulses & 0xFF);
        line[4] = (uint8_t)((freq >> 8) & 0xFF);
        line[5] = (uint8_t)(freq & 0xFF);
        line[6] = ambit_actinic_to_dac(actinic, par_coef);
        line[7] = 1;                              /* subsampling: every point */
    }

    /* Run metadata (small, bounded), built from the packed segment array. */
    {
        int off = snprintf(metadata_json, meta_cap, "{\"segments\":[");
        for (int i = 0; i < nseg && off > 0 && off < (int)meta_cap; i++) {
            const uint8_t *line = run_arr + i * 8;
            off += snprintf(metadata_json + off, meta_cap - off,
                            "%s{\"pulses\":%u,\"freq\":%u,\"actinic\":%u}", (i == 0) ? "" : ",",
                            ((unsigned)line[2] << 8) | line[3],
                            ((unsigned)line[4] << 8) | line[5], (unsigned)line[6]);
        }
        if (off < 0 || off > (int)meta_cap - 3) off = (int)meta_cap - 3;
        snprintf(metadata_json + off, meta_cap - off, "]}");
    }
    return run_arr;
}

/* Decode an AMBIT FSM response into the reserved JSON payload, optionally store
 * it as one event, free `resp`, and push the Lua result table (or nil+reason).
 * Shared by ambit.run and ambit.fetch so both produce identical events;
 * `cmd_name` is recorded as the event's cmd_raw, using the AMBIT firmware's
 * own ASCII command vocabulary (do_command.h): the binary run (cmd 21) and
 * the async trigger/fetch pair (cmds 22/24) are all the "arrun" measurement
 * — sync vs async is transport, not stimulus, so both store "arrun". */
static int ambit_decode_store_push(lua_State *L, uart_sensor_response_t *resp,
                                   uint8_t ch, bool store, int64_t start_ms,
                                   int64_t end_ms, const char *metadata_json,
                                   const char *cmd_name)
{
    uint8_t narr = resp->array_count;
    if (narr == 0) {
        uart_sensor_response_free(resp);
        return lua_push_nil_reason(L, "ambit run returned no arrays");
    }

    /* Reserve the JSON payload buffer once (lazily here; normally already done at
     * module register, while the heap was contiguous) and reuse it every run. */
    if (s_ambit_payload == NULL) {
        s_ambit_payload = malloc(AMBIT_RUN_PAYLOAD_CAP);
        if (s_ambit_payload == NULL) {
            uart_sensor_response_free(resp);
            return luaL_error(L, "out of memory reserving %dB payload (free=%d, largest=%d)",
                              (int)AMBIT_RUN_PAYLOAD_CAP,
                              (int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                              (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }

    char  *pbuf = s_ambit_payload;
    size_t cap  = AMBIT_RUN_PAYLOAD_CAP;
    size_t off  = 0;
    bool   trunc = false;
    size_t fluor_len = 0;
    double leaf_temp_c = 0.0;
    bool   have_temp = false;

#define AMB_APP(...) do {                                                      \
        int _n = snprintf(pbuf + off, off < cap ? cap - off : 0, __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= cap - off) trunc = true;                   \
        else off += (size_t)_n;                                               \
    } while (0)

    AMB_APP("{");
    for (uint8_t a = 0; a < narr && !trunc; a++) {
        const uart_data_array_t *arr = &resp->arrays[a];
        char tagbuf[12];
        const char *tag = ambit_array_tag(arr->index);
        if (tag == NULL) { snprintf(tagbuf, sizeof tagbuf, "arr%u", arr->index); tag = tagbuf; }

        AMB_APP("%s\"%s\":[", a == 0 ? "" : ",", tag);
        for (uint16_t i = 0; i < arr->length && !trunc; i++) {
            if (arr->index == 0) {                  /* ENV → leaf temp degC */
                double t = (int16_t)(arr->data[i] & 0xFFFF) / 100.0;
                if (!have_temp) { leaf_temp_c = t; have_temp = true; }
                AMB_APP("%s%.2f", i ? "," : "", t);
            } else {
                AMB_APP("%s%u", i ? "," : "", (unsigned)arr->data[i]);
            }
        }
        AMB_APP("]");
        if (arr->index == 1) fluor_len = arr->length;
    }
    AMB_APP("}");
#undef AMB_APP

    if (trunc) {
        ESP_LOGW(LUA_RUNNER_TAG,
                 "ambit.run payload truncated at %dB — raise AMBIT_RUN_PAYLOAD_CAP",
                 (int)cap);
    }

    int stored = 0;
    if (store && !trunc) {
        char chan[12];
        snprintf(chan, sizeof chan, "uart_%u", (unsigned)ch);
        int64_t mid = 0;
        if (cmd_next_measure_id(&mid).status == ESP_OK) {
            measurement_event_desc_t d = {
                .measure_id    = mid,
                .channel       = chan,
                .device        = "ambit",
                .tag           = MEASUREMENT_TAG_MEASUREMENT,
                .cmd_raw       = cmd_name,
                .start_ms      = start_ms,
                .end_ms        = end_ms,
                .metadata_json = metadata_json,
                .payload_json  = s_ambit_payload,
            };
            if (cmd_store_event(&d).status == ESP_OK) {
                stored = 1;
            }
        }
    }

    uart_sensor_response_free(resp);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)fluor_len);    lua_setfield(L, -2, "points");
    lua_pushinteger(L, stored);                    lua_setfield(L, -2, "stored");
    lua_pushnumber(L, leaf_temp_c);                lua_setfield(L, -2, "leaf_temp");
    lua_pushinteger(L, (lua_Integer)narr);         lua_setfield(L, -2, "arrays");
    return 1;
}

static int l_ambit_run(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
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

    char metadata_json[768];
    uint8_t *run_arr = ambit_build_run_arr(L, 2, ch, nseg, metadata_json, sizeof metadata_json);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t start_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    /* Binary AMBYTE run (cmd 21): the ambit streams each result array back over
     * the FSM handshake as raw uint32 LE — no ASCII text buffer, no cJSON tree.
     * Arrays land in resp.arrays[] keyed by index (see ambit fw pam_send_results). */
    uart_sensor_response_t resp;
    cmd_result_t res = cmd_ambit_run(ch, run_arr, (uint8_t)nseg, persist,
                                     allow_interrupt, &resp, timeout_ms);
    free(run_arr);

    gettimeofday(&tv, NULL);
    int64_t end_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    if (res.status != ESP_OK) {
        uart_sensor_response_free(&resp);
        return lua_push_nil_reason(L, res.message);
    }
    return ambit_decode_store_push(L, &resp, ch, store, start_ms, end_ms,
                                   metadata_json, "arrun");
}

/* ambit.trigger(ch, segments [, opts]) — start a retained (async) run on `ch`
 * and return true once the ambit acks (cmd 22). The run executes on the ambit
 * to completion; collect it later with ambit.poll + ambit.fetch. Stashes the
 * run's metadata + start time per channel for the eventual store.
 *   opts : { persist=false, interrupt=false, timeout_ms=3000 } */
static int l_ambit_trigger(lua_State *L)
{
    uint8_t ch = (uint8_t)luaL_checkinteger(L, 1);
    if (ch >= UART_SENSOR_NUM_CHANNELS)
        return luaL_error(L, "channel must be 0-%d", UART_SENSOR_NUM_CHANNELS - 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int nseg = (int)luaL_len(L, 2);
    if (nseg <= 0 || nseg > 16)
        return luaL_error(L, "segments: 1-16 lines required (got %d)", nseg);

    uint8_t  persist        = 0;
    bool     allow_interrupt = false;
    uint32_t timeout_ms     = 3000;   /* covers wake + ack only */
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "persist");
        if (!lua_isnil(L, -1)) persist = lua_toboolean(L, -1) ? 1 : 0;
        lua_pop(L, 1);
        lua_getfield(L, 3, "interrupt");
        if (!lua_isnil(L, -1)) allow_interrupt = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "timeout_ms");
        if (lua_isnumber(L, -1)) timeout_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    uint8_t *run_arr = ambit_build_run_arr(L, 2, ch, nseg,
                                           s_ambit_meta[ch], sizeof s_ambit_meta[ch]);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_ambit_start_ms[ch] = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    cmd_result_t res = cmd_ambit_trigger(ch, run_arr, (uint8_t)nseg, persist,
                                         allow_interrupt, timeout_ms);
    free(run_arr);
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

    uart_sensor_response_t resp;
    cmd_result_t res = cmd_ambit_fetch(ch, &resp, timeout_ms);
    if (res.status != ESP_OK) {
        uart_sensor_response_free(&resp);
        return lua_push_nil_reason(L, res.message);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t end_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    return ambit_decode_store_push(L, &resp, ch, store,
                                   s_ambit_start_ms[ch], end_ms, s_ambit_meta[ch],
                                   "arrun");
}

/* ── ambit.* bindings ────────────────────────────────────────────────
 * Exposes the AMBIT binary uart_query (moved from device.uart_query) and the
 * high-level array run. The typed ambit_* helpers still live under device;
 * consolidating them all here is a separate cleanup. */
static void lua_register_ambit_module(lua_State *L)
{
    /* Reserve the run payload buffer up front, while the heap is still
     * contiguous — avoids a fragmentation-time failure on the first run. */
    if (s_ambit_payload == NULL) {
        s_ambit_payload = malloc(AMBIT_RUN_PAYLOAD_CAP);
        if (s_ambit_payload == NULL) {
            ESP_LOGW(LUA_RUNNER_TAG,
                     "ambit.run payload reserve (%dB) failed; will retry per-run",
                     (int)AMBIT_RUN_PAYLOAD_CAP);
        }
    }

    static const luaL_Reg ambit_api[] = {
        {"uart_query", l_ambit_uart_query},
        {"run",        l_ambit_run},
        {"trigger",    l_ambit_trigger},   /* parallel protocol: start retained run */
        {"poll",       l_ambit_poll},      /*   "    "    "    : query async state  */
        {"fetch",      l_ambit_fetch},     /*   "    "    "    : collect + store     */
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
    cmd_read_rtc(&t);
    return (int64_t)t;
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
}

static void lua_runner_task(void *arg)
{
    (void)arg;

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        ESP_LOGE(LUA_RUNNER_TAG, "Failed to create Lua state");
        goto done;
    }

    luaL_openlibs(L);
    lua_register_device_module(L);
    lua_register_db_module(L);
    lua_register_ambit_module(L);
    lua_register_sync_module(L);

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

    BaseType_t created = xTaskCreate(
        lua_runner_task,
        LUA_RUNNER_TASK_NAME,
        LUA_RUNNER_TASK_STACK,
        NULL,
        LUA_RUNNER_TASK_PRIORITY,
        &s_lua_task_handle
    );
    if (created != pdPASS) {
        s_lua_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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

/* Execute the production driver and frontends. The device_commands port shim
 * delegates to the real driver; only I2C, worker submission and reply transport
 * are faked. Every bus operation asserts lock ownership. */
#include "mp2731.h"
#include "cli_vin_limit.h"
#include "vin_limit_command.h"
#include "device_commands.h"
#include "i2c_bus.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t reg01 = 6;
static int locked, locks, unlocks, writes, reads, submits, calls;
static int port_error, lock_error, write_error, read_error, mismatch, probe_error, reject_submit;
static char last[1024];
static void (*job)(void *);
static void *argument;
static int read_mutex;
static int64_t clock_us;

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "test_error"; }
void vTaskDelay(TickType_t ticks) { clock_us += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return clock_us; }
void esp_rom_delay_us(unsigned us) { clock_us += us; }
void h_critical_enter(portMUX_TYPE *m) { assert(!m->held); m->held = 1; }
void h_critical_exit(portMUX_TYPE *m) { assert(m->held); m->held = 0; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &read_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    (void)wait; assert(m == &read_mutex && !read_mutex); read_mutex = 1; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{
    assert(m == &read_mutex && read_mutex); read_mutex = 0; return pdTRUE;
}
esp_err_t i2c_bus_get_port(i2c_port_t *port) { *port = 0; return port_error; }
esp_err_t i2c_bus_probe_addr(i2c_port_t port, uint8_t addr)
{
    assert(port == 0 && addr == 0x4b); return probe_error;
}
esp_err_t i2c_bus_lock(TickType_t timeout)
{
    assert(timeout > 0 && !locked);
    if (lock_error) return lock_error;
    locked = 1; locks++; return ESP_OK;
}
esp_err_t i2c_bus_unlock(void) { assert(locked); locked = 0; unlocks++; return ESP_OK; }
esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t addr,
    const uint8_t *buf, size_t size, TickType_t timeout)
{
    assert(locked && port == 0 && addr == 0x4b && size == 2 && timeout > 0);
    assert(buf[0] == 1 && !(buf[1] & 0x80)); /* never write REGISTER RESET */
    writes++;
    if (write_error) return write_error;
    reg01 = buf[1]; return ESP_OK;
}
esp_err_t i2c_master_write_read_device(i2c_port_t port, uint8_t addr,
    const uint8_t *reg, size_t reg_size, uint8_t *out, size_t size, TickType_t timeout)
{
    assert(locked && port == 0 && addr == 0x4b && reg_size == 1 && size == 1 && timeout > 0);
    assert(*reg == 1); reads++;
    if (read_error) return read_error;
    *out = reg01 + mismatch; return ESP_OK;
}

static cmd_result_t command_result(esp_err_t status, const power_vin_limit_t *limit)
{
    cmd_result_t result = {.status = status};
    if (status == ESP_OK) {
        snprintf(result.message, sizeof result.message, "VIN limit=%u mV REG01=0x%02X", limit->mv, limit->reg01);
    } else {
        snprintf(result.message, sizeof result.message, "test_error=%d", status);
    }
    return result;
}
cmd_result_t cmd_get_vin_limit(power_vin_limit_t *out)
{
    calls++; return command_result(mp2731_get_vin_limit(out), out);
}
cmd_result_t cmd_set_vin_limit(uint16_t mv, power_vin_limit_t *out)
{
    calls++; return command_result(mp2731_set_vin_limit(mv, out), out);
}
static void reply(const char *json) { assert(strlen(json) < sizeof last); strcpy(last, json); }
static bool submit(void (*run)(void *), void *arg)
{
    submits++;
    if (reject_submit) return false;
    assert(!job); job = run; argument = arg; return true;
}
static void request(const char *json)
{
    cJSON *root = cJSON_Parse(json); assert(root);
    assert(vin_limit_command_handle(root)); cJSON_Delete(root);
}
static void execute(void)
{
    assert(job); void (*run)(void *) = job; job = NULL; run(argument);
    assert(!locked && locks == unlocks);
}
static cJSON *response(bool ok)
{
    cJSON *root = cJSON_Parse(last); assert(root);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")) == ok);
    if (!ok) assert(cJSON_GetObjectItemCaseSensitive(root, "mv") == NULL);
    return root;
}
static void expect_error(const char *detail)
{
    cJSON *root = response(false);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(root, "detail")->valuestring, detail));
    cJSON_Delete(root);
}
static int cli(int argc, char **argv) { return cli_cmd_vin_limit(argc, argv); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *set = "{\"type\":\"set_vin_limit\",\"id\":\"test-\\\"quoted\\\"\",\"mv\":4000}";
    const char *get = "{\"type\":\"get_vin_limit\",\"id\":\"read\"}";
    power_vin_limit_t limit = {.mv = 123, .reg01 = 123};
    if (!strcmp(argv[1], "invalid")) {
        assert(mp2731_get_vin_limit(&limit) == ESP_ERR_INVALID_STATE);
        assert(mp2731_set_vin_limit(4000, &limit) == ESP_ERR_INVALID_STATE);
        probe_error = ESP_FAIL; assert(mp2731_init() == ESP_FAIL && !mp2731_is_ready());
        assert(mp2731_get_vin_limit(&limit) == ESP_ERR_INVALID_STATE);
        assert(mp2731_set_vin_limit(4000, &limit) == ESP_ERR_INVALID_STATE);
        assert(!locks && !reads && !writes);
        probe_error = 0;
    }
    assert(mp2731_init() == ESP_OK && mp2731_is_ready());
    assert(!writes); /* initialization never changes the charger's default */
    vin_limit_command_config_t config = {.reply = reply, .submit = submit};
    vin_limit_command_init(&config);

    if (!strcmp(argv[1], "limits")) {
        assert(mp2731_get_vin_limit(&limit) == ESP_OK && limit.mv == 4300 && limit.reg01 == 6);
        for (unsigned mv = 3700; mv <= 15200; mv += 100) {
            int previous_locks = locks;
            assert(mp2731_set_vin_limit(mv, &limit) == ESP_OK);
            assert(limit.mv == mv && limit.reg01 == (mv - 3700) / 100);
            assert(locks == previous_locks + 1 && locks == unlocks);
            assert(mp2731_get_vin_limit(&limit) == ESP_OK && limit.mv == mv);
        }
    } else if (!strcmp(argv[1], "invalid")) {
        unsigned values[] = {0, 4, 3699, 3750, 3999, 4001, 4050, 15201, 65535};
        for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
            assert(mp2731_set_vin_limit(values[i], &limit) == ESP_ERR_INVALID_ARG);
        }
        assert(mp2731_get_vin_limit(NULL) == ESP_ERR_INVALID_ARG);
        assert(mp2731_set_vin_limit(4000, NULL) == ESP_ERR_INVALID_ARG);
        assert(!locks && !reads && !writes);
    } else if (!strcmp(argv[1], "failures")) {
        port_error = ESP_FAIL; assert(mp2731_set_vin_limit(4000, &limit) == ESP_FAIL);
        assert(mp2731_get_vin_limit(&limit) == ESP_FAIL && !locks);
        port_error = 0; lock_error = ESP_ERR_TIMEOUT;
        assert(mp2731_set_vin_limit(4000, &limit) == ESP_ERR_TIMEOUT);
        assert(mp2731_get_vin_limit(&limit) == ESP_ERR_TIMEOUT && !locks);
        lock_error = 0; write_error = ESP_FAIL;
        assert(mp2731_set_vin_limit(4000, &limit) == ESP_FAIL && reads == 0);
        write_error = 0; read_error = ESP_ERR_TIMEOUT;
        assert(mp2731_set_vin_limit(4000, &limit) == ESP_ERR_TIMEOUT);
        assert(mp2731_get_vin_limit(&limit) == ESP_ERR_TIMEOUT);
        read_error = 0; mismatch = 1;
        assert(mp2731_set_vin_limit(4000, &limit) == ESP_ERR_INVALID_RESPONSE);
        assert(limit.mv == 123 && limit.reg01 == 123);
        mismatch = 0; reg01 = 0x80;
        assert(mp2731_get_vin_limit(&limit) == ESP_ERR_INVALID_RESPONSE);
        reg01 = 116; assert(mp2731_get_vin_limit(&limit) == ESP_ERR_INVALID_RESPONSE);
    } else if (!strcmp(argv[1], "cli")) {
        char *read_args[] = {"vin_limit"};
        assert(cli(1, read_args) == 0 && !writes);
        char *set_args[] = {"vin_limit", "set", "4000"};
        assert(cli(3, set_args) == 0 && reg01 == 3);
        /* strtoul negates in unsigned arithmetic: these host-width magnitudes
         * wrap to 4000 without ERANGE. Keep the explicit ESP32 (32-bit long)
         * case too, since the usual 64-bit host alone would reject that one. */
        char negative_wrap[64], spaced_negative_wrap[64];
        snprintf(negative_wrap, sizeof negative_wrap, "-%lu", ULONG_MAX - 3999UL);
        snprintf(spaced_negative_wrap, sizeof spaced_negative_wrap, " \t-%lu", ULONG_MAX - 3999UL);
        const char *values[] = {"", "4.0", "4000.1", "4050", "-1", "999999999999999999999999",
            "65536", "4000junk", "-4294963296", " \t-4294963296", negative_wrap, spaced_negative_wrap};
        int previous_calls = calls;
        for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
            set_args[2] = (char *)values[i]; assert(cli(3, set_args) == 1);
        }
        assert(cli(2, set_args) == 1 && calls == previous_calls);
        read_error = ESP_FAIL; assert(cli(1, read_args) == 1);
    } else if (!strcmp(argv[1], "mqtt")) {
        request(get); assert(job && !reads && !writes && !calls); execute();
        cJSON *root = response(true);
        assert(cJSON_GetObjectItemCaseSensitive(root, "mv")->valueint == 4300);
        cJSON_Delete(root);
        request(set); assert(writes == 0); execute();
        root = response(true);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "set_vin_limit_result"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring, "test-\"quoted\""));
        assert(cJSON_GetObjectItemCaseSensitive(root, "mv")->valueint == 4000);
        assert(cJSON_GetObjectItemCaseSensitive(root, "reg01")->valueint == 3);
        cJSON_Delete(root);
        request(set); execute(); assert(reg01 == 3); /* QoS1 duplicate is harmless */
        request(get); execute(); assert(writes == 2);
    } else if (!strcmp(argv[1], "mqtt_invalid")) {
        const char *values[] = {"null", "true", "\"4000\"", "[]", "{}", "4", "4000.5", "4050", "3699", "15201", "1e99", "-1"};
        for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
            char json[256]; snprintf(json, sizeof json, "{\"type\":\"set_vin_limit\",\"id\":\"bad\",\"mv\":%s}", values[i]);
            request(json); expect_error("invalid_mv");
        }
        request("{\"type\":\"set_vin_limit\",\"id\":\"bad\"}"); expect_error("invalid_mv");
        request("{\"type\":\"get_vin_limit\"}"); expect_error("invalid_id");
        request("{\"type\":\"get_vin_limit\",\"id\":\"\"}"); expect_error("invalid_id");
        char json[256], id[66]; memset(id, 'x', 65); id[65] = '\0';
        snprintf(json, sizeof json, "{\"type\":\"get_vin_limit\",\"id\":\"%s\"}", id);
        request(json); expect_error("invalid_id"); assert(!submits && !calls && !writes);
        id[64] = '\0'; snprintf(json, sizeof json, "{\"type\":\"get_vin_limit\",\"id\":\"%s\"}", id);
        request(json); execute(); cJSON *root = response(true); cJSON_Delete(root);
        root = cJSON_Parse("{\"type\":\"other\"}"); assert(!vin_limit_command_handle(root)); cJSON_Delete(root);
    } else {
        assert(!strcmp(argv[1], "mqtt_failures"));
        request(set); request(get); expect_error("busy"); assert(submits == 1 && !writes);
        execute();
        reject_submit = 1; request(get); expect_error("queue_unavailable"); assert(!job);
        reject_submit = 0; mismatch = 1; request(set); execute(); expect_error("test_error");
        mismatch = 0; request(get); execute(); cJSON *root = response(true); cJSON_Delete(root);
        read_error = ESP_ERR_TIMEOUT; request(get); execute(); expect_error("test_error");
        config.submit = NULL; vin_limit_command_init(&config);
        request(get); expect_error("queue_unavailable");
    }
    assert(!locked && locks == unlocks && !job);
    puts("OK");
    return 0;
}

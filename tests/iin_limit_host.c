/* Execute the production driver and frontends for the MP2731 input current
 * limit (docs/charger-iin-limit.md). The device_commands port shim delegates
 * to the real driver; only I2C, worker submission and reply transport are
 * faked. Every bus operation asserts lock ownership, and every REG00 write
 * asserts that bits 7:6 (EN_HIZ, EN_ILIM) are exactly what the chip held. */
#include "mp2731.h"
#include "cli_iin_limit.h"
#include "iin_limit_command.h"
#include "device_commands.h"
#include "i2c_bus.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t reg00 = 0x48;   /* every field unit: EN_ILIM=1, 500 mA */
static int booting;   /* inside mp2731_init (boot reconciliation reads allowed) */
static int locked, locks, unlocks, writes, reads, submits, calls;
static int port_error, lock_error, write_error, read_error, mismatch, probe_error, reject_submit;
static int read_error_at;      /* fail the Nth read (1-based) of a set, 0 = never */
static uint8_t flip_after_write; /* XOR applied to the chip after the write lands */
static int set_reads;
static char last[1024];
static void (*job)(void *);
static void *argument;
static int read_mutex;
static int64_t clock_us;

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "test_error"; }
#include "esp_system.h"
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
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
    locked = 1; locks++; set_reads = 0; return ESP_OK;
}
esp_err_t i2c_bus_unlock(void) { assert(locked); locked = 0; unlocks++; return ESP_OK; }
esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t addr,
    const uint8_t *buf, size_t size, TickType_t timeout)
{
    assert(locked && port == 0 && addr == 0x4b && size == 2 && timeout > 0);
    assert(buf[0] == 0);                          /* REG00 only */
    assert((buf[1] & 0xC0) == (reg00 & 0xC0));    /* HIZ and EN_ILIM untouched */
    assert(set_reads == 1);                       /* written only after a read */
    writes++;
    if (write_error) return write_error;
    reg00 = buf[1] ^ flip_after_write; return ESP_OK;
}
esp_err_t i2c_master_write_read_device(i2c_port_t port, uint8_t addr,
    const uint8_t *reg, size_t reg_size, uint8_t *out, size_t size, TickType_t timeout)
{
    assert(locked && port == 0 && addr == 0x4b && reg_size == 1 && size == 1 && timeout > 0);
    /* Boot reconciliation (HAR-310) reads REG09/REG0B once, inside
     * mp2731_init only (POR 0); anywhere else they are unexpected reads. */
    if (booting && (*reg == 0x09 || *reg == 0x0B)) { *out = 0; return ESP_OK; }
    assert(*reg == 0); reads++; set_reads++;
    if (read_error) return read_error;
    if (read_error_at && set_reads == read_error_at) return ESP_ERR_TIMEOUT;
    *out = (uint8_t)(reg00 + (set_reads > 1 ? mismatch : 0)); return ESP_OK;
}

static cmd_result_t command_result(esp_err_t status, const power_iin_limit_t *limit)
{
    cmd_result_t result = {.status = status};
    if (status == ESP_OK) {
        snprintf(result.message, sizeof result.message, "IIN register limit=%u mA REG00=0x%02X",
                 limit->ma, limit->reg00);
    } else {
        snprintf(result.message, sizeof result.message, "test_error=%d", status);
    }
    return result;
}
cmd_result_t cmd_get_iin_limit(power_iin_limit_t *out)
{
    calls++; return command_result(mp2731_get_iin_limit(out), out);
}
cmd_result_t cmd_set_iin_limit(uint16_t ma, power_iin_limit_t *out)
{
    calls++; return command_result(mp2731_set_iin_limit(ma, out), out);
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
    assert(iin_limit_command_handle(root)); cJSON_Delete(root);
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
    if (!ok) {
        assert(cJSON_GetObjectItemCaseSensitive(root, "ma") == NULL);
        assert(cJSON_GetObjectItemCaseSensitive(root, "reg00") == NULL);
        assert(cJSON_GetObjectItemCaseSensitive(root, "en_ilim") == NULL);
    }
    return root;
}
static void expect_error(const char *detail)
{
    cJSON *root = response(false);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(root, "detail")->valuestring, detail));
    cJSON_Delete(root);
}
static int number(cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    assert(cJSON_IsNumber(item)); return item->valueint;
}
static bool flag(cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    assert(cJSON_IsBool(item)); return cJSON_IsTrue(item);
}
static int cli(int argc, char **argv) { return cli_cmd_iin_limit(argc, argv); }

static esp_err_t boot_init(void)
{
    const esp_err_t err = mp2731_init();
    booting = 0;
    return err;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *set = "{\"type\":\"set_iin_limit\",\"id\":\"test-\\\"quoted\\\"\",\"ma\":1000}";
    const char *get = "{\"type\":\"get_iin_limit\",\"id\":\"read\"}";
    power_iin_limit_t limit = {.ma = 123, .reg00 = 123};
    if (!strcmp(argv[1], "invalid")) {
        assert(mp2731_get_iin_limit(&limit) == ESP_ERR_INVALID_STATE);
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_INVALID_STATE);
        probe_error = ESP_FAIL; booting = 1; assert(boot_init() == ESP_FAIL && !mp2731_is_ready());
        assert(mp2731_get_iin_limit(&limit) == ESP_ERR_INVALID_STATE);
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_INVALID_STATE);
        assert(!locks && !reads && !writes);
        probe_error = 0;
    }
    booting = 1; assert(boot_init() == ESP_OK && mp2731_is_ready());
    assert(!writes); /* initialization never changes the charger's default */
    locks = unlocks = 0; /* boot reconciliation reads are not IIN-limit traffic */
    iin_limit_command_config_t config = {.reply = reply, .submit = submit};
    iin_limit_command_init(&config);

    if (!strcmp(argv[1], "limits")) {
        assert(mp2731_get_iin_limit(&limit) == ESP_OK && limit.ma == 500 && limit.reg00 == 0x48);
        /* Every code under every HIZ/EN_ILIM combination: the high bits are
         * carried through from the chip (asserted in the write fake too). */
        const uint8_t highs[] = {0x00, 0x40, 0x80, 0xC0};
        for (unsigned h = 0; h < sizeof highs; h++) {
            reg00 = (uint8_t)(highs[h] | 0x08);
            for (unsigned ma = 100; ma <= 3250; ma += 50) {
                int previous_locks = locks;
                assert(mp2731_set_iin_limit(ma, &limit) == ESP_OK);
                assert(limit.ma == ma && limit.reg00 == (highs[h] | (ma - 100) / 50));
                assert(reg00 == limit.reg00);
                assert(locks == previous_locks + 1 && locks == unlocks);
                assert(mp2731_get_iin_limit(&limit) == ESP_OK && limit.ma == ma);
            }
        }
        /* Every raw byte decodes (bits 7:6 are state, not corruption). */
        for (unsigned raw = 0; raw <= 0xFF; raw++) {
            reg00 = (uint8_t)raw;
            assert(mp2731_get_iin_limit(&limit) == ESP_OK);
            assert(limit.reg00 == raw && limit.ma == 100 + (raw & 0x3F) * 50);
        }
    } else if (!strcmp(argv[1], "invalid")) {
        unsigned values[] = {0, 50, 99, 101, 125, 149, 501, 525, 1025, 3251, 3300, 65535};
        for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
            assert(mp2731_set_iin_limit(values[i], &limit) == ESP_ERR_INVALID_ARG);
        }
        assert(mp2731_get_iin_limit(NULL) == ESP_ERR_INVALID_ARG);
        assert(mp2731_set_iin_limit(1000, NULL) == ESP_ERR_INVALID_ARG);
        assert(!locks && !reads && !writes && reg00 == 0x48);
    } else if (!strcmp(argv[1], "failures")) {
        port_error = ESP_FAIL; assert(mp2731_set_iin_limit(1000, &limit) == ESP_FAIL);
        assert(mp2731_get_iin_limit(&limit) == ESP_FAIL && !locks);
        port_error = 0; lock_error = ESP_ERR_TIMEOUT;
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_TIMEOUT);
        assert(mp2731_get_iin_limit(&limit) == ESP_ERR_TIMEOUT && !locks);
        lock_error = 0;
        /* The read that supplies bits 7:6 fails: nothing may be written. */
        read_error_at = 1; assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_TIMEOUT);
        assert(!writes && reg00 == 0x48);
        read_error_at = 0; write_error = ESP_FAIL;
        int previous_reads = reads;
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_FAIL && reads == previous_reads + 1);
        write_error = 0; read_error_at = 2;   /* write lands, readback fails */
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_TIMEOUT && reg00 == 0x52);
        read_error_at = 0; reg00 = 0x48; read_error = ESP_ERR_TIMEOUT;
        assert(mp2731_get_iin_limit(&limit) == ESP_ERR_TIMEOUT);
        read_error = 0; mismatch = 1;
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_INVALID_RESPONSE);
        mismatch = 0;
        /* EN_ILIM dropped by the chip in the window (a watchdog reset would
         * do the opposite, but either way the byte is not what was written). */
        reg00 = 0x48; flip_after_write = 0x40;
        assert(mp2731_set_iin_limit(1000, &limit) == ESP_ERR_INVALID_RESPONSE);
        flip_after_write = 0;
        assert(limit.ma == 123 && limit.reg00 == 123); /* never filled on failure */
        assert(!locked && locks == unlocks);
    } else if (!strcmp(argv[1], "cli")) {
        char *read_args[] = {"iin_limit"};
        assert(cli(1, read_args) == 0 && !writes);
        char *set_args[] = {"iin_limit", "set", "1000"};
        assert(cli(3, set_args) == 0 && reg00 == 0x52);
        set_args[2] = "500"; assert(cli(3, set_args) == 0 && reg00 == 0x48);
        /* strtoul negates in unsigned arithmetic: these host-width magnitudes
         * wrap to 1000 without ERANGE. Keep the explicit ESP32 (32-bit long)
         * case too, since a 64-bit host alone would reject that one. */
        char negative_wrap[64], spaced_negative_wrap[64];
        snprintf(negative_wrap, sizeof negative_wrap, "-%lu", ULONG_MAX - 999UL);
        snprintf(spaced_negative_wrap, sizeof spaced_negative_wrap, " \t-%lu", ULONG_MAX - 999UL);
        const char *values[] = {"", "1.0", "1000.5", "1025", "-1", "0", "99", "3251", "3300",
            "999999999999999999999999", "65536", "1000junk", "1A", "-4294966296", " \t-4294966296",
            negative_wrap, spaced_negative_wrap};
        int previous_calls = calls;
        int previous_writes = writes;
        for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
            set_args[2] = (char *)values[i]; assert(cli(3, set_args) == 1);
        }
        assert(cli(2, set_args) == 1 && calls == previous_calls && writes == previous_writes);
        char *other[] = {"iin_limit", "get", "1000"};
        assert(cli(3, other) == 1);
        read_error = ESP_FAIL; assert(cli(1, read_args) == 1);
    } else if (!strcmp(argv[1], "mqtt")) {
        request(get); assert(job && !reads && !writes && !calls); execute();
        cJSON *root = response(true);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "iin_limit_result"));
        assert(number(root, "ma") == 500 && number(root, "reg00") == 0x48);
        assert(flag(root, "en_ilim") && !flag(root, "hiz"));
        cJSON_Delete(root);
        request(set); assert(writes == 0); execute();
        root = response(true);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "set_iin_limit_result"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring, "test-\"quoted\""));
        assert(number(root, "ma") == 1000 && number(root, "reg00") == 0x52);
        assert(flag(root, "en_ilim") && !flag(root, "hiz"));
        cJSON_Delete(root);
        request(set); execute(); assert(reg00 == 0x52); /* QoS1 duplicate is harmless */
        request(get); execute(); assert(writes == 2);
        /* HIZ on, EN_ILIM off: both reported, both preserved by a set. */
        reg00 = 0x88;
        request("{\"type\":\"set_iin_limit\",\"id\":\"hiz\",\"ma\":3250}"); execute();
        root = response(true);
        assert(number(root, "ma") == 3250 && number(root, "reg00") == 0xBF);
        assert(!flag(root, "en_ilim") && flag(root, "hiz"));
        cJSON_Delete(root);
    } else if (!strcmp(argv[1], "mqtt_invalid")) {
        const char *values[] = {"null", "true", "\"1000\"", "[]", "{}", "50", "99", "1000.5", "1025",
                                "3251", "3300", "1e99", "-1", "-1000", "0"};
        for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
            char json[256]; snprintf(json, sizeof json, "{\"type\":\"set_iin_limit\",\"id\":\"bad\",\"ma\":%s}", values[i]);
            request(json); expect_error("invalid_ma");
        }
        request("{\"type\":\"set_iin_limit\",\"id\":\"bad\"}"); expect_error("invalid_ma");
        /* The VIN command's field name is not accepted here. */
        request("{\"type\":\"set_iin_limit\",\"id\":\"bad\",\"mv\":1000}"); expect_error("invalid_ma");
        request("{\"type\":\"get_iin_limit\"}"); expect_error("invalid_id");
        request("{\"type\":\"get_iin_limit\",\"id\":\"\"}"); expect_error("invalid_id");
        request("{\"type\":\"get_iin_limit\",\"id\":7}"); expect_error("invalid_id");
        char json[256], id[66]; memset(id, 'x', 65); id[65] = '\0';
        snprintf(json, sizeof json, "{\"type\":\"get_iin_limit\",\"id\":\"%s\"}", id);
        request(json); expect_error("invalid_id"); assert(!submits && !calls && !writes);
        id[64] = '\0'; snprintf(json, sizeof json, "{\"type\":\"get_iin_limit\",\"id\":\"%s\"}", id);
        request(json); execute(); cJSON *root = response(true); cJSON_Delete(root);
        const char *others[] = {"{\"type\":\"other\"}", "{\"type\":\"get_vin_limit\",\"id\":\"x\"}",
                                "{\"type\":\"set_vin_limit\",\"id\":\"x\",\"mv\":4000}", "{\"type\":3}", "{}"};
        for (unsigned i = 0; i < sizeof others / sizeof *others; i++) {
            root = cJSON_Parse(others[i]); assert(!iin_limit_command_handle(root)); cJSON_Delete(root);
        }
    } else {
        assert(!strcmp(argv[1], "mqtt_failures"));
        request(set); request(get); expect_error("busy"); assert(submits == 1 && !writes);
        execute();
        reject_submit = 1; request(get); expect_error("queue_unavailable"); assert(!job);
        reject_submit = 0; mismatch = 1; request(set); execute(); expect_error("test_error");
        mismatch = 0; request(get); execute(); cJSON *root = response(true); cJSON_Delete(root);
        read_error = ESP_ERR_TIMEOUT; request(get); execute(); expect_error("test_error");
        config.submit = NULL; iin_limit_command_init(&config);
        request(get); expect_error("queue_unavailable");
    }
    assert(!locked && locks == unlocks && !job);
    puts("OK");
    return 0;
}

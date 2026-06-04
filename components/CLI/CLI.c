#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "CLI.h"
#include "driver/i2c.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ambit_protocol.h"
#include "device_commands.h"
#include "i2c_bus.h"
#include "wifi_manager.h"

static const uint8_t CLI_I2C_SCAN_FIRST_ADDR = 0x08;
static const uint8_t CLI_I2C_SCAN_LAST_ADDR = 0x77;
static const uint8_t CLI_RTC_I2C_ADDR = (uint8_t)(0xA6 >> 1);
static const uint8_t CLI_BME280_ADDR_PRIMARY = 0x77;
static const uint8_t CLI_BME280_ADDR_SECONDARY = 0x76;
static const TickType_t CLI_I2C_SCAN_TIMEOUT_TICKS = pdMS_TO_TICKS(50);
static const TickType_t CLI_I2C_SCAN_LOCK_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);
static esp_console_repl_t *s_cli_repl = NULL;
static bool s_cli_commands_registered = false;

static esp_err_t cli_i2c_probe_locked(i2c_port_t port, uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);

    const esp_err_t err = i2c_master_cmd_begin(port, cmd, CLI_I2C_SCAN_TIMEOUT_TICKS);
    i2c_cmd_link_delete(cmd);
    return err;
}

static const char *cli_i2c_label_for_addr(uint8_t addr)
{
    switch (addr) {
        case CLI_RTC_I2C_ADDR:
            return "PCF2131 RTC";
        case CLI_BME280_ADDR_PRIMARY:
            return "BME280 candidate (primary)";
        case CLI_BME280_ADDR_SECONDARY:
            return "BME280 candidate (secondary)";
        default:
            return NULL;
    }
}

static int cli_cmd_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: status\r\n");
        return 1;
    }

    bool bme_ready = false;
    bool rtc_ready = false;
    time_t rtc_time = 0;
    cmd_result_t res = cmd_device_status(&bme_ready, &rtc_ready, &rtc_time);
    printf("CLI status:\r\n");
    printf(" - BME280 ready: %s\r\n", bme_ready ? "yes" : "no");
    printf(" - RTC ready: %s\r\n", rtc_ready ? "yes" : "no");

    if (rtc_ready) {
        struct tm tm_now;
        char now_s[32] = {0};
        localtime_r(&rtc_time, &tm_now);
        strftime(now_s, sizeof(now_s), "%Y-%m-%d %H:%M:%S", &tm_now);
        printf(" - RTC now: %s (%lld)\r\n", now_s, (long long)rtc_time);
    }
    (void)res;
    return 0;
}

static int cli_cmd_rtc(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: rtc\r\n");
        return 1;
    }

    time_t now = 0;
    cmd_result_t res = cmd_read_rtc(&now);
    if (res.status != ESP_OK) {
        printf("RTC read error: %s\r\n", res.message);
        return 1;
    }

    struct tm tm_now;
    char now_s[32] = {0};
    localtime_r(&now, &tm_now);
    strftime(now_s, sizeof(now_s), "%Y-%m-%d %H:%M:%S", &tm_now);
    printf("RTC: %s (%lld)\r\n", now_s, (long long)now);
    return 0;
}

static int cli_cmd_red(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: red <0|1>\r\n");
        return 1;
    }

    cmd_result_t res;
    if (strcmp(argv[1], "0") == 0) {
        res = cmd_set_rgb(0, 0, 0);
        if (res.status == ESP_OK) {
            printf("Red LED off\r\n");
            return 0;
        }
    } else if (strcmp(argv[1], "1") == 0) {
        res = cmd_set_rgb(5, 0, 0);
        if (res.status == ESP_OK) {
            printf("Red LED on\r\n");
            return 0;
        }
    } else {
        printf("Usage: red <0|1>\r\n");
        return 1;
    }

    printf("Status LED error: %s\r\n", res.message);
    return 1;
}

static int cli_cmd_read_env(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: read_env\r\n");
        return 1;
    }

    float temp = 0, hum = 0, pres = 0;
    cmd_result_t res = cmd_read_env(&temp, &hum, &pres);
    if (res.status != ESP_OK) {
        printf("read_env failed: %s\r\n", res.message);
        return 1;
    }
    printf("T=%.2fC  H=%.1f%%  P=%.0fPa\r\n", temp, hum, pres);
    return 0;
}

static int cli_cmd_cert_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: cert_status\r\n");
        return 1;
    }

    cmd_result_t res = cmd_cert_status();
    printf("%s\r\n", res.message);
    return (res.status == ESP_OK) ? 0 : 1;
}

static int cli_cmd_test_t52(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* PASS = function returned at all (no crash/panic).
     * Both ESP_OK (port available, succeeded) and any error (port unavailable or
     * MQTT disconnected) are graceful outcomes. Only a panic is a hard FAIL,
     * which would prevent reaching the result line below. */
#define T52_CHECK(label, call) do { \
    cmd_result_t _r = (call); \
    const char *_tag = (_r.status == ESP_OK)               ? "OK  " : \
                       (_r.status == ESP_ERR_NOT_SUPPORTED) ? "N/A " : \
                       (_r.status == ESP_ERR_INVALID_STATE) ? "DIS " : "ERR "; \
    printf("[T5.2] %-42s PASS [%s] %s\r\n", label, _tag, _r.message); \
    pass++; total++; \
} while (0)

    int pass = 0, total = 0;

    /* ── MQTT port guards ──────────────────────────────────────────── */
    T52_CHECK("cmd_mqtt_status",
              cmd_mqtt_status());
    T52_CHECK("cmd_mqtt_publish",
              cmd_mqtt_publish("test/topic", "test"));
    T52_CHECK("cmd_mqtt_publish_raw",
              cmd_mqtt_publish_raw("test"));
    T52_CHECK("cmd_mqtt_publish_next_event",
              cmd_mqtt_publish_next_event());
    T52_CHECK("cmd_cert_status",
              cmd_cert_status());

    /* ── Persistence port guards (event-document model) ────────────── */
    T52_CHECK("cmd_store_event",
              cmd_store_event(999999LL, "", "test", 1, 1, NULL,
                              "{\"temperature\":1.0}"));

    int64_t nid = 0;
    T52_CHECK("cmd_next_measure_id",
              cmd_next_measure_id(&nid));

#undef T52_CHECK

    /* If we reached here, all calls returned — no crash. */
    printf("[T5.2] Result: %d/%d returned gracefully (no panic)\r\n", pass, total);
    printf("[T5.2] Tags: OK=succeeded  N/A=port NULL  DIS=MQTT disconnected  ERR=other error\r\n");
    return 0;
}

static int cli_cmd_mqtt_pub(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: mqtt_pub <payload>\r\n");
        return 1;
    }

    /* Rejoin all tokens — allows JSON with spaces without requiring quotes */
    static char s_payload[480];
    int pos = 0;
    for (int i = 1; i < argc && pos < (int)sizeof(s_payload) - 1; i++) {
        if (i > 1 && pos < (int)sizeof(s_payload) - 2) {
            s_payload[pos++] = ' ';
        }
        int n = snprintf(s_payload + pos, sizeof(s_payload) - (size_t)pos, "%s", argv[i]);
        if (n > 0) pos += n;
    }
    s_payload[pos] = '\0';

    cmd_result_t res = cmd_mqtt_publish_raw(s_payload);
    printf("%s\r\n", res.message);
    return (res.status == ESP_OK) ? 0 : 1;
}

static int cli_cmd_i2cscan(int argc, char **argv)
{
    (void)argv;

    if (argc != 1) {
        printf("Usage: i2cscan\r\n");
        return 1;
    }

    i2c_port_t port = I2C_NUM_MAX;
    esp_err_t err = i2c_bus_get_port(&port);
    if (err != ESP_OK) {
        printf("I2C bus unavailable: %s\r\n", esp_err_to_name(err));
        return 1;
    }

    err = i2c_bus_lock(CLI_I2C_SCAN_LOCK_TIMEOUT_TICKS);
    if (err != ESP_OK) {
        printf("I2C bus lock failed: %s\r\n", esp_err_to_name(err));
        return 1;
    }

    int found_count = 0;
    bool scan_error = false;
    bool found_rtc = false;
    bool found_bme_primary = false;
    bool found_bme_secondary = false;

    printf("I2C scan results on port %d (7-bit addresses 0x%02X-0x%02X):\r\n",
           (int)port,
           CLI_I2C_SCAN_FIRST_ADDR,
           CLI_I2C_SCAN_LAST_ADDR);

    for (uint8_t addr = CLI_I2C_SCAN_FIRST_ADDR; addr <= CLI_I2C_SCAN_LAST_ADDR; ++addr) {
        const esp_err_t probe_err = cli_i2c_probe_locked(port, addr);
        if (probe_err == ESP_OK) {
            const char *label = cli_i2c_label_for_addr(addr);
            printf(" - 0x%02X", addr);
            if (label != NULL) {
                printf("  %s", label);
            }
            printf("\r\n");

            ++found_count;
            found_rtc = found_rtc || (addr == CLI_RTC_I2C_ADDR);
            found_bme_primary = found_bme_primary || (addr == CLI_BME280_ADDR_PRIMARY);
            found_bme_secondary = found_bme_secondary || (addr == CLI_BME280_ADDR_SECONDARY);
            continue;
        }

        if (probe_err != ESP_FAIL) {
            printf("Scan aborted at 0x%02X: %s\r\n", addr, esp_err_to_name(probe_err));
            scan_error = true;
            break;
        }
    }

    const esp_err_t unlock_err = i2c_bus_unlock();
    if (unlock_err != ESP_OK) {
        printf("I2C bus unlock failed: %s\r\n", esp_err_to_name(unlock_err));
        return 1;
    }

    if (scan_error) {
        return 1;
    }

    if (found_count == 0) {
        printf("No I2C devices acknowledged in the scanned range.\r\n");
    } else {
        printf("Found %d device(s).\r\n", found_count);
    }

    printf(
        "Summary: RTC=%s  0x%02X=%s  0x%02X=%s\r\n",
        found_rtc ? "yes" : "no",
        CLI_BME280_ADDR_SECONDARY,
        found_bme_secondary ? "yes" : "no",
        CLI_BME280_ADDR_PRIMARY,
        found_bme_primary ? "yes" : "no");
    return 0;
}

static int cli_cmd_ping_uart(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: ping_uart <channel 0-3>\r\n");
        return 1;
    }

    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }

    bool connected = false;
    cmd_result_t res = cmd_uart_ping((uint8_t)ch, &connected);
    printf("%s\r\n", res.message);
    return (res.status == ESP_OK) ? 0 : 1;
}

static int cli_cmd_uart_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: uart_status\r\n");
        return 1;
    }

    cmd_result_t res = cmd_uart_status();
    printf("%s\r\n", res.message);
    return (res.status == ESP_OK) ? 0 : 1;
}

/* uart_query <ch> <message> [timeout_ms=1000]
 *   Sends the ASCII line <message> (single token, LF-terminated) over channel
 *   <ch> and prints the reply. timeout_ms is optional and defaults to 1000.
 *   Always passes save=false so manual probing doesn't pollute the
 *   measurements DB. */
static int cli_cmd_uart_query(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        printf("Usage: uart_query <ch> <message> [timeout_ms=1000]\r\n");
        return 1;
    }

    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }

    const char *cmd = argv[2];

    /* Optional trailing timeout; default 1 s. */
    int timeout_ms = (argc == 4) ? atoi(argv[3]) : 1000;
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }

    char   resp[256];
    size_t resp_len = 0;
    cmd_result_t res = cmd_uart_text_query((uint8_t)ch, cmd, "\n",
                                           (uint32_t)timeout_ms, false,
                                           resp, sizeof(resp), &resp_len);

    if (res.status == ESP_ERR_TIMEOUT) {
        printf("ch%d: timeout after %dms (no response)\r\n", ch, timeout_ms);
        return 1;
    }
    if (res.status != ESP_OK) {
        printf("ch%d: %s\r\n", ch, res.message);
        return 1;
    }
    printf("ch%d (%u bytes): %s\r\n", ch, (unsigned)resp_len, resp);
    return 0;
}

static int cli_cmd_ambit_temp(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: ambit_temp <channel 0-3>\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    float leaf = 0, chip = 0;
    cmd_result_t res = cmd_ambit_get_temp((uint8_t)ch, &leaf, &chip);
    if (res.status != ESP_OK) {
        printf("Error: %s\r\n", res.message);
        return 1;
    }
    printf("AMBIT%d leaf=%.1fC  chip=%.1fC\r\n", ch + 1, leaf, chip);
    return 0;
}

static int cli_cmd_ambit_spec(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: ambit_spec <channel 0-3>\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    uint16_t spec[10] = {0};
    float par = 0;
    cmd_result_t res = cmd_ambit_get_spec((uint8_t)ch, spec, &par);
    if (res.status != ESP_OK) {
        printf("Error: %s\r\n", res.message);
        return 1;
    }
    printf("AMBIT%d spectrum:", ch + 1);
    for (int i = 0; i < 10; i++) printf(" %u", spec[i]);
    printf("  PAR=%.2f\r\n", par);
    return 0;
}

static int cli_cmd_ambit_info(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: ambit_info <channel 0-3> <1=cal|2=fw|3=meta>\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    int type = atoi(argv[2]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS || type < 1 || type > 3) {
        printf("Channel 0-%d, type 1-3\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    uint8_t buf[256];
    size_t len = 0;
    cmd_result_t res = cmd_ambit_get_info((uint8_t)ch, (uint8_t)type,
                                           buf, sizeof(buf), &len);
    if (res.status != ESP_OK) {
        printf("Error: %s\r\n", res.message);
        return 1;
    }
    printf("AMBIT%d info(%d): %u bytes\r\n", ch + 1, type, (unsigned)len);
    if (type == AMBIT_INFO_CALIBRATION && len >= sizeof(ambit_calibration_t)) {
        const ambit_calibration_t *cal = (const ambit_calibration_t *)buf;
        printf("  Name:       %.20s\r\n", cal->ambit_name);
        printf("  Actinic:    %.4f\r\n", cal->actinic_coef);
        printf("  Spec coef:  %.4f\r\n", cal->spec_coef);
        printf("  Emissivity: %.4f\r\n", cal->mlx_emissivity);
        printf("  Sun coef:   %.4f\r\n", cal->sun_coef);
        printf("  Temp off/slope: %.2f / %.2f\r\n", cal->temp_offset, cal->temp_slope);
        printf("  ADPD offsets: %lu %lu %lu %lu %lu %lu\r\n",
               (unsigned long)cal->adpd[0], (unsigned long)cal->adpd[1],
               (unsigned long)cal->adpd[2], (unsigned long)cal->adpd[3],
               (unsigned long)cal->adpd[4], (unsigned long)cal->adpd[5]);
    }
    if (type == AMBIT_INFO_FW && len >= sizeof(ambit_fw_info_t)) {
        const ambit_fw_info_t *fw = (const ambit_fw_info_t *)buf;
        printf("  FW version: %u.%u.%u\r\n", fw->major, fw->minor, fw->batch);
        printf("  MAC:        %016llX\r\n", (unsigned long long)fw->mac);
        printf("  Build date: %.12s\r\n", fw->fw_date);
        printf("  Flash size: %lu\r\n", (unsigned long)fw->size);
    }
    if (type == AMBIT_INFO_METADATA && len >= sizeof(ambit_metadata_t)) {
        const ambit_metadata_t *meta = (const ambit_metadata_t *)buf;
        printf("  GPS:  lon=%.6f  lat=%.6f\r\n", meta->lon, meta->lat);
        printf("  Alt:  %.1f m  (acc=%.1f  vacc=%.1f)\r\n",
               meta->alt, meta->acc, meta->vacc);
        printf("  Time: %lu\r\n", (unsigned long)meta->time);
        printf("  Info: %.60s%s\r\n", meta->info1,
               strlen(meta->info1) > 60 ? "..." : "");
        printf("  EOF:  %u\r\n", meta->eof_mark);
    }
    return 0;
}

static int cli_cmd_ambit_blink(int argc, char **argv)
{
    if (argc != 4) {
        printf("Usage: ambit_blink <channel 0-3> <ambit_id 0-3> <intensity 5-253>\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    int id = atoi(argv[2]);
    int intensity = atoi(argv[3]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    cmd_result_t res = cmd_ambit_blink((uint8_t)ch, (uint8_t)id, (uint8_t)intensity);
    printf("%s\r\n", res.message);
    return (res.status == ESP_OK) ? 0 : 1;
}

static int cli_cmd_wifi_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Clearing Wi-Fi credentials and provisioning state...\r\n");
    wifi_manager_clear_provisioning();  /* does not return — calls esp_restart() */
    return 0;
}

/* PWM <duty 0-100> [freq_hz=10000] [enable 0|1=1]
 *   Drives a PWM on GPIO4 via LEDC. duty accepts floats (e.g. 37.5). freq
 *   defaults to 10000 Hz. enable defaults to 1; pass 0 to stop output. */
static int cli_cmd_pwm(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: PWM <duty 0-100> [freq_hz=10000] [enable 0|1=1]\r\n");
        return 1;
    }

    float duty = strtof(argv[1], NULL);
    if (duty < 0.0f || duty > 100.0f) {
        printf("duty must be 0..100\r\n");
        return 1;
    }

    uint32_t freq = 10000;
    if (argc >= 3) {
        long f = strtol(argv[2], NULL, 10);
        if (f <= 0) {
            printf("freq must be > 0\r\n");
            return 1;
        }
        freq = (uint32_t)f;
    }

    bool enable = (argc >= 4) ? (atoi(argv[3]) != 0) : true;

    cmd_result_t res = cmd_pwm(duty, freq, enable);
    printf("%s\r\n", res.message);
    return (res.status == ESP_OK) ? 0 : 1;
}

static int cli_cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Rebooting...\r\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the message flush before reset */
    esp_restart();
    return 0; /* not reached */
}

static esp_err_t cli_register_commands(void)
{
    if (s_cli_commands_registered) {
        return ESP_OK;
    }

    static const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "print peripheral readiness and current RTC time",
        .func = cli_cmd_status,
    };
    static const esp_console_cmd_t rtc_cmd = {
        .command = "rtc",
        .help = "print current RTC timestamp",
        .func = cli_cmd_rtc,
    };
    static const esp_console_cmd_t red_cmd = {
        .command = "red",
        .help = "red <0|1>  turn red LED off/on",
        .func = cli_cmd_red,
    };
    static const esp_console_cmd_t read_env_cmd = {
        .command = "read_env",
        .help = "read BME280 temperature, humidity, pressure",
        .func = cli_cmd_read_env,
    };
    static const esp_console_cmd_t i2cscan_cmd = {
        .command = "i2cscan",
        .help = "scan the shared I2C bus for responding 7-bit addresses",
        .func = cli_cmd_i2cscan,
    };
    static const esp_console_cmd_t cert_status_cmd = {
        .command = "cert_status",
        .help = "print whether TLS certificates have been provisioned",
        .func = cli_cmd_cert_status,
    };
    static const esp_console_cmd_t mqtt_pub_cmd = {
        .command = "mqtt_pub",
        .help = "mqtt_pub <payload>  publish string to <topic_root>/<device_id>/cli",
        .func = cli_cmd_mqtt_pub,
    };
    static const esp_console_cmd_t test_t52_cmd = {
        .command = "test_t52",
        .help = "T5.2: verify all port-guarded cmd_* return gracefully when ports are unavailable",
        .func = cli_cmd_test_t52,
    };
    static const esp_console_cmd_t ping_uart_cmd = {
        .command = "ping_uart",
        .help    = "ping_uart <0-3>  check if AMBIT sensor on channel is connected",
        .func    = cli_cmd_ping_uart,
    };
    static const esp_console_cmd_t uart_status_cmd = {
        .command = "uart_status",
        .help    = "show connection state of all 4 UART sensor channels",
        .func    = cli_cmd_uart_status,
    };
    static const esp_console_cmd_t uart_query_cmd = {
        .command = "uart_query",
        .help    = "uart_query <ch> <message> [timeout_ms=1000]  ASCII line query (LF-terminated, save=false)",
        .func    = cli_cmd_uart_query,
    };
    static const esp_console_cmd_t ambit_temp_cmd = {
        .command = "ambit_temp",
        .help    = "ambit_temp <0-3>  read leaf+chip temperature from AMBIT sensor",
        .func    = cli_cmd_ambit_temp,
    };
    static const esp_console_cmd_t ambit_spec_cmd = {
        .command = "ambit_spec",
        .help    = "ambit_spec <0-3>  read spectrum + PAR from AMBIT sensor",
        .func    = cli_cmd_ambit_spec,
    };
    static const esp_console_cmd_t ambit_info_cmd = {
        .command = "ambit_info",
        .help    = "ambit_info <0-3> <1=cal|2=fw|3=meta>  read sensor info",
        .func    = cli_cmd_ambit_info,
    };
    static const esp_console_cmd_t ambit_blink_cmd = {
        .command = "ambit_blink",
        .help    = "ambit_blink <0-3> <id> <brightness>  blink AMBIT LED",
        .func    = cli_cmd_ambit_blink,
    };
    static const esp_console_cmd_t wifi_reset_cmd = {
        .command = "wifi_reset",
        .help    = "clear Wi-Fi credentials + provisioning flag and reboot",
        .func    = cli_cmd_wifi_reset,
    };
    static const esp_console_cmd_t pwm_cmd = {
        .command = "PWM",
        .help    = "PWM <duty 0-100> [freq_hz=10000] [enable 0|1=1]  drive PWM on GPIO4",
        .func    = cli_cmd_pwm,
    };
    static const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help    = "restart the device",
        .func    = cli_cmd_reboot,
    };

    esp_err_t err = esp_console_cmd_register(&status_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&rtc_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&red_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&read_env_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&i2cscan_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&cert_status_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&mqtt_pub_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&test_t52_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ping_uart_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&uart_status_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&uart_query_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_temp_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_spec_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_info_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_blink_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&wifi_reset_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&pwm_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&reboot_cmd);
    if (err != ESP_OK) {
        return err;
    }

    s_cli_commands_registered = true;
    return ESP_OK;
}

static void cli_reset_after_failure(void)
{
    (void)esp_console_deinit();
    s_cli_commands_registered = false;
    s_cli_repl = NULL;
}

static esp_err_t cli_create_repl(const esp_console_repl_config_t *repl_config)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usb_serial_jtag_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_serial_jtag(&usb_serial_jtag_config, repl_config, &s_cli_repl);
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t usb_cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_cdc(&usb_cdc_config, repl_config, &s_cli_repl);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    return esp_console_new_repl_uart(&uart_config, repl_config, &s_cli_repl);
#else
    (void)repl_config;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t cli_start(void)
{
    if (s_cli_repl != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ambyte> ";
    repl_config.max_cmdline_length = 512;

    esp_err_t err = cli_create_repl(&repl_config);
    if (err != ESP_OK) {
        s_cli_repl = NULL;
        return err;
    }

    err = cli_register_commands();
    if (err != ESP_OK) {
        cli_reset_after_failure();
        return err;
    }

    err = esp_console_start_repl(s_cli_repl);
    if (err != ESP_OK) {
        cli_reset_after_failure();
        return err;
    }

    return ESP_OK;
}

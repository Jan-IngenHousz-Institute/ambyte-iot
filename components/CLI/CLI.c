#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "CLI.h"
#include "driver/i2c.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ambit_ota.h"
#include "ambit_flash.h"
#include "ambit_protocol.h"
#include "device_commands.h"
#include "uart_sensors.h"
#include "lua_runner.h"
#include "time_sync.h"
#include "i2c_bus.h"
#include "pcf2131tfy_rtc_api.h"
#include "sd_logger.h"
#include "wifi_manager.h"

#ifdef CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#endif

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
    cmd_device_status(&bme_ready, &rtc_ready, &rtc_time);
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

    /* Wi-Fi connection + provisioning state. */
    bool provisioned = false;
    wifi_manager_is_provisioned(&provisioned);
    printf(" - Wi-Fi: %s (provisioned: %s)\r\n",
           wifi_manager_is_connected() ? "connected" : "disconnected",
           provisioned ? "yes" : "no");

    /* STA MAC — board identity (e.g. to spot a duplicate MQTT client ID). */
    const char *mac = device_commands_get_mac();
    printf(" - MAC: %s\r\n", (mac && mac[0]) ? mac : "(unavail)");

    /* Event DB + sync backlog. */
    bool    db_online = false;
    int64_t total = 0, pending = 0, next_id = 0;
    cmd_result_t dres = cmd_db_status(&db_online, &total, &pending, &next_id);
    if (dres.status == ESP_OK) {
        printf(" - DB: %s\r\n", db_online ? "online" : "offline");
        if (db_online) {
            printf("   events: %lld total, %lld pending sync, next id %lld\r\n",
                   (long long)total, (long long)pending, (long long)next_id);
        }
    } else {
        printf(" - DB: %s\r\n", dres.message);
    }

    /* Battery / input power (MP2731 charger). */
    power_reading_t pw;
    cmd_result_t pres = cmd_read_power(&pw);
    if (pres.status == ESP_OK) {
        static const char *const charge_str[] = {"idle", "pre-charge", "charging", "charged"};
        printf(" - Power: Vbat %.2f V, Vin %.2f V, Vsys %.2f V, Iin %u mA, Icharge %u mA\r\n",
               pw.battery_mv / 1000.0, pw.input_mv / 1000.0, pw.system_mv / 1000.0,
               pw.input_ma, pw.charge_ma);
        printf("   source: %s, charge: %s, publish gate: %s\r\n",
               pw.input_present ? "external" : "battery",
               charge_str[pw.charge_status & 0x03],
               device_commands_publish_power_ok() ? "OPEN" : "CLOSED");
    } else {
        printf(" - Power: %s\r\n", pres.message);
    }
    return 0;
}

/* Print the current RTC time (read back through the clock port). */
static int cli_print_rtc(void)
{
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

/* rtc                                  → print current time
 * rtc set <epoch_seconds>              → set from UTC epoch seconds
 * rtc set <YYYY-MM-DD> <HH:MM:SS>      → set from a UTC date + time
 * Setting clears the oscillator-stop flag, so this is how a factory-fresh RTC
 * is brought online; the system clock is re-synced immediately. */
static int cli_cmd_rtc(int argc, char **argv)
{
    if (argc == 1) {
        return cli_print_rtc();
    }

    if (strcmp(argv[1], "set") != 0) {
        printf("Usage: rtc | rtc set <epoch> | rtc set <YYYY-MM-DD> <HH:MM:SS>  (UTC)\r\n");
        return 1;
    }

    struct tm tm_utc = {0};

    if (argc == 3) {
        /* rtc set <epoch_seconds> */
        char *end = NULL;
        const long long epoch = strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || epoch < 0) {
            printf("rtc set: invalid epoch '%s'\r\n", argv[2]);
            return 1;
        }
        const time_t t = (time_t)epoch;
        gmtime_r(&t, &tm_utc);
    } else if (argc == 4) {
        /* rtc set <YYYY-MM-DD> <HH:MM:SS> (UTC) */
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) != 3 ||
            sscanf(argv[3], "%d:%d:%d", &h, &mi, &s) < 2) {
            printf("rtc set: bad date/time; expected <YYYY-MM-DD> <HH:MM:SS>\r\n");
            return 1;
        }
        if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
            h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
            printf("rtc set: value out of range\r\n");
            return 1;
        }
        tm_utc.tm_year = y - 1900;
        tm_utc.tm_mon  = mo - 1;
        tm_utc.tm_mday = d;
        tm_utc.tm_hour = h;
        tm_utc.tm_min  = mi;
        tm_utc.tm_sec  = s;
    } else {
        printf("Usage: rtc set <epoch> | rtc set <YYYY-MM-DD> <HH:MM:SS>  (UTC)\r\n");
        return 1;
    }

    const esp_err_t err = pcf2131tfy_rtc_set_time(&tm_utc);
    if (err != ESP_OK) {
        printf("rtc set failed: %s\r\n", esp_err_to_name(err));
        return 1;
    }
    printf("RTC set; ");
    return cli_print_rtc();
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

static int cli_cmd_record_env(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: record_env\r\n");
        return 1;
    }

    int64_t mid = 0;
    cmd_result_t res = cmd_record_env(&mid, NULL);
    if (res.status != ESP_OK) {
        printf("record_env failed: %s\r\n", res.message);
        return 1;
    }
    printf("%s\r\n", res.message);   /* "recorded env id=N: T=…C H=…% P=…Pa" */
    return 0;
}

static int cli_cmd_log_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: log_status\r\n");
        return 1;
    }

    bool   active = false;
    size_t buffered = 0, dropped = 0, file_bytes = 0;
    sd_logger_stats(&active, &buffered, &dropped, &file_bytes);
    printf("SD log: %s\r\n", active ? "writing" : "paused (no SD?)");
    printf(" - file: /sdcard/logs/ambyte.log (%u bytes)\r\n", (unsigned)file_bytes);
    printf(" - buffered: %u bytes, dropped: %u bytes\r\n",
           (unsigned)buffered, (unsigned)dropped);
    return 0;
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
 *   Transport-only — text queries never store (schema-v2 rule). */
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
                                           (uint32_t)timeout_ms,
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

/* sync [sun | loc <lat> <lon> [tz] | interval <s> [phase] | clock <hh> <mm> |
 *       weekly <d,d,…> <hh> <mm> | sunrise|sunset [offset_s]]
 * Computes scheduling triggers against the RTC (see components/time_sync). */
static int cli_cmd_sync(int argc, char **argv)
{
    time_t now_t = 0;
    cmd_read_rtc(&now_t);
    const int64_t now = (int64_t)now_t;

    const char *sub = (argc >= 2) ? argv[1] : "sun";

    if (strcmp(sub, "sun") == 0) {
        int y, mo, d, h, mi, s, wd;
        time_sync_localtime(now, &y, &mo, &d, &h, &mi, &s, &wd);
        double lat, lon; int tz;
        time_sync_get_location(&lat, &lon, &tz);
        char srs[8] = "--:--", sss[8] = "--:--";
        int64_t u; int hh, mm;
        if (time_sync_sun_on_date(now, TIME_SYNC_SUNRISE, &u) == ESP_OK) {
            time_sync_localtime(u, NULL, NULL, NULL, &hh, &mm, NULL, NULL);
            snprintf(srs, sizeof(srs), "%02d:%02d", hh, mm);
        }
        if (time_sync_sun_on_date(now, TIME_SYNC_SUNSET, &u) == ESP_OK) {
            time_sync_localtime(u, NULL, NULL, NULL, &hh, &mm, NULL, NULL);
            snprintf(sss, sizeof(sss), "%02d:%02d", hh, mm);
        }
        printf("RTC now: %04d-%02d-%02d %02d:%02d:%02d (wday %d)\r\n", y, mo, d, h, mi, s, wd);
        printf("loc: lat=%.4f lon=%.4f tz=%+d\r\n", lat, lon, tz);
        printf("sunrise %s  sunset %s  (RTC time)\r\n", srs, sss);
        return 0;
    }

    if (strcmp(sub, "loc") == 0) {
        if (argc >= 4) {
            int tz; time_sync_get_location(NULL, NULL, &tz);
            if (argc >= 5) tz = atoi(argv[4]);
            time_sync_set_location(strtod(argv[2], NULL), strtod(argv[3], NULL), tz);
        }
        double lat, lon; int tz;
        time_sync_get_location(&lat, &lon, &tz);
        printf("loc: lat=%.4f lon=%.4f tz=%+d\r\n", lat, lon, tz);
        return 0;
    }

    int64_t secs = -1;
    if (strcmp(sub, "interval") == 0 && argc >= 3) {
        secs = time_sync_until_interval(now, atoll(argv[2]), (argc >= 4) ? atoll(argv[3]) : 0);
    } else if (strcmp(sub, "clock") == 0 && argc >= 4) {
        secs = time_sync_until_clock(now, atoi(argv[2]), atoi(argv[3]), 0);
    } else if (strcmp(sub, "weekly") == 0 && argc >= 5) {
        uint8_t mask = 0;
        char buf[64], *save = NULL;
        strncpy(buf, argv[2], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            int b = time_sync_day_bit(tok);
            if (b >= 0) mask |= (uint8_t)(1u << b);
        }
        secs = time_sync_until_weekly(now, mask, atoi(argv[3]), atoi(argv[4]));
    } else if (strcmp(sub, "sunrise") == 0 || strcmp(sub, "sunset") == 0) {
        int ev = (strcmp(sub, "sunset") == 0) ? TIME_SYNC_SUNSET : TIME_SYNC_SUNRISE;
        secs = time_sync_until_sun(now, ev, (argc >= 3) ? atoll(argv[2]) : 0);
    } else {
        printf("Usage: sync [sun | loc <lat> <lon> [tz] | interval <s> [phase] | "
               "clock <hh> <mm> | weekly <d,d> <hh> <mm> | sunrise|sunset [offset_s]]\r\n");
        return 1;
    }

    if (secs < 0) {
        printf("no trigger (bad args, or polar day/night)\r\n");
        return 1;
    }
    int y, mo, d, h, mi, s, wd;
    time_sync_localtime(now + secs, &y, &mo, &d, &h, &mi, &s, &wd);
    printf("next in %llds  -> %04d-%02d-%02d %02d:%02d:%02d\r\n",
           (long long)secs, y, mo, d, h, mi, s);
    return 0;
}

#ifdef CONFIG_HEAP_TRACING_STANDALONE
/* Diagnostic: standalone heap-leak tracing. `heaptrace start` → run a few minutes
 * while the leak accumulates → `heaptrace dump` prints every allocation made since
 * start that is still un-freed, with the allocating call stack. addr2line the
 * call-stack PCs against firmware.elf to find the leaking function. The ring is
 * static .bss so it doesn't compete with the heap under test. */
#define CLI_HEAP_TRACE_RECORDS 300
static heap_trace_record_t s_heap_trace_buf[CLI_HEAP_TRACE_RECORDS];
static bool s_heap_trace_inited = false;

static int cli_cmd_heaptrace(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: heaptrace <start|stop|dump>\r\n");
        return 1;
    }
    if (!s_heap_trace_inited) {
        esp_err_t e = heap_trace_init_standalone(s_heap_trace_buf, CLI_HEAP_TRACE_RECORDS);
        if (e != ESP_OK) {
            printf("heaptrace: init failed: %s\r\n", esp_err_to_name(e));
            return 1;
        }
        s_heap_trace_inited = true;
    }
    if (strcmp(argv[1], "start") == 0) {
        esp_err_t e = heap_trace_start(HEAP_TRACE_LEAKS);
        printf("heaptrace start: %s (ring=%d records). Let the leak run, then 'heaptrace dump'.\r\n",
               esp_err_to_name(e), CLI_HEAP_TRACE_RECORDS);
    } else if (strcmp(argv[1], "stop") == 0) {
        printf("heaptrace stop: %s\r\n", esp_err_to_name(heap_trace_stop()));
    } else if (strcmp(argv[1], "dump") == 0) {
        heap_trace_dump();   /* prints un-freed allocations + call stacks to console */
    } else {
        printf("Usage: heaptrace <start|stop|dump>\r\n");
        return 1;
    }
    return 0;
}
#endif /* CONFIG_HEAP_TRACING_STANDALONE */

/* ── OTA dual-slot / rollback validation (OTA Stage 1 HW test) ────────────
 * Temporary bench tooling to prove the dual-OTA partition table + rollback
 * end-to-end with no server: clone the running image into the other slot,
 * switch boot, and exercise the PENDING_VERIFY → commit/rollback transitions.
 * Reusable as Stage-3 confidence; remove or keep as a diagnostic. */

static const char *cli_ota_state_str(esp_ota_img_states_t st)
{
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        default:                         return "UNDEFINED";
    }
}

static int cli_cmd_ota_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);

    printf("running: %-7s @ 0x%06lx (%lu KB)  state=%s\r\n",
           run ? run->label : "?", run ? (unsigned long)run->address : 0UL,
           run ? (unsigned long)(run->size / 1024) : 0UL, cli_ota_state_str(st));
    if (boot) printf("boot:    %-7s @ 0x%06lx\r\n", boot->label, (unsigned long)boot->address);
    if (next) printf("next:    %-7s @ 0x%06lx  (ota_selftest clone target)\r\n",
                     next->label, (unsigned long)next->address);
    return 0;
}

static int cli_cmd_ota_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (run == NULL || next == NULL || run == next) {
        printf("ota_selftest: need two OTA slots (run=%s next=%s) — single-OTA layout?\r\n",
               run ? run->label : "?", next ? next->label : "?");
        return 1;
    }
    printf("cloning %s (0x%06lx) -> %s (0x%06lx), %lu KB ...\r\n",
           run->label, (unsigned long)run->address, next->label,
           (unsigned long)next->address, (unsigned long)(run->size / 1024));

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(next, run->size, &h);
    if (err != ESP_OK) { printf("ota_begin: %s\r\n", esp_err_to_name(err)); return 1; }

    const size_t CHUNK = 4096;
    uint8_t *buf = malloc(CHUNK);
    if (buf == NULL) { esp_ota_abort(h); printf("oom\r\n"); return 1; }
    size_t off = 0;
    while (off < run->size) {
        size_t n = (run->size - off < CHUNK) ? (run->size - off) : CHUNK;
        err = esp_partition_read(run, off, buf, n);
        if (err != ESP_OK) break;
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) break;
        off += n;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        printf("clone failed @ %u B: %s\r\n", (unsigned)off, esp_err_to_name(err));
        return 1;
    }
    if ((err = esp_ota_end(h)) != ESP_OK) {
        printf("ota_end (image validate): %s\r\n", esp_err_to_name(err));
        return 1;
    }
    if ((err = esp_ota_set_boot_partition(next)) != ESP_OK) {
        printf("set_boot: %s\r\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: %s set as boot. `reboot` -> boots it in PENDING_VERIFY.\r\n", next->label);
    printf("  commit: `ota_mark_valid` (then it stays).\r\n");
    printf("  rollback: `reboot` again WITHOUT mark_valid -> reverts to %s.\r\n", run->label);
    return 0;
}

static int cli_cmd_ota_mark_valid(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    printf("mark_app_valid_cancel_rollback: %s\r\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* Stream a new AMBIT (C3) firmware image over UART. Non-blocking: queues the
 * request and the ambit_ota worker downloads to SD, suspends Lua+MQTT, streams,
 * and the sensor reboots into the new slot. Watch the log for progress. Use
 * `ambit_info <ch> 2` before/after to read the version. */
static int cli_cmd_ambit_ota(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: ambit_ota <channel 0-3 | all> <firmware-url>\r\n");
        printf("  'all' = every present channel (sequential). URL must be a direct .bin\r\n");
        printf("  (raw.githubusercontent.com/...), not a /blob//tree/ page. Suspends Lua+MQTT.\r\n");
        return 1;
    }
    uint8_t ch;
    if (strcmp(argv[1], "all") == 0) {
        ch = AMBIT_OTA_CH_ALL;
    } else {
        int c = atoi(argv[1]);
        if (c < 0 || c >= UART_SENSOR_NUM_CHANNELS) {
            printf("Channel 0-%d or 'all'\r\n", UART_SENSOR_NUM_CHANNELS - 1);
            return 1;
        }
        ch = (uint8_t)c;
    }
    esp_err_t err = ambit_ota_request(ch, argv[2], NULL /* CLI: no dedupe id */);
    if (err != ESP_OK) {
        printf("ambit_ota: could not queue (%s)\r\n", esp_err_to_name(err));
        return 1;
    }
    printf("AMBIT OTA queued (%s) — watch the log; the sensor reboots on success.\r\n",
           (ch == AMBIT_OTA_CH_ALL) ? "all" : argv[1]);
    return 0;
}

/* Phase-1 bench test for the host-driven AMBIT flasher: drive an AMBIT into the
 * ESP32-C3 ROM serial download mode (`enter`) or reset it back to its app
 * (`run`) using the ambyte reset/boot lines. After `enter` the chip STAYS in
 * download mode — verify with esptool on the FFC tap:
 *   esptool --chip esp32c3 --port <COM> --before no_reset --after no_reset chip_id
 * Stop Lua first (`lua stop`) so the shared UART bus is free. */
static int cli_cmd_ambit_dl(int argc, char **argv)
{
    if (argc != 3 ||
        (strcmp(argv[2], "enter") != 0 && strcmp(argv[2], "run") != 0)) {
        printf("Usage: ambit_dl <0-3> <enter|run>\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    esp_err_t err = uart_sensors_flash_session_begin((uint8_t)ch, 2000);
    if (err != ESP_OK) {
        printf("bus busy (%s) — try `lua stop` first\r\n", esp_err_to_name(err));
        return 1;
    }
    if (strcmp(argv[2], "enter") == 0) {
        err = uart_sensors_enter_download((uint8_t)ch);
        printf("ch%d enter_download: %s — chip should be in ROM download mode; "
               "connect esptool on the FFC tap\r\n", ch, esp_err_to_name(err));
    } else {
        err = uart_sensors_run_app((uint8_t)ch);
        printf("ch%d run_app: %s — chip reset into its application\r\n",
               ch, esp_err_to_name(err));
    }
    uart_sensors_flash_session_end((uint8_t)ch);
    return (err == ESP_OK) ? 0 : 1;
}

/* Host-driven ROM flasher probe: force the AMBIT on <ch> into ESP32-C3 ROM
 * download mode over the FFC, connect via esp-serial-flasher, read chip + MAC,
 * then reset it back to its app. No PC/tap needed — the ambyte is the flasher.
 * Stop Lua first (`lua stop`) so the shared UART bus is free. */
static int cli_cmd_ambit_probe(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: ambit_probe <0-3>   (stop Lua first: `lua stop`)\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    ambit_flash_probe_result_t r;
    esp_err_t err = ambit_flash_probe((uint8_t)ch, &r);
    if (err == ESP_OK && r.connected) {
        printf("ch%d: AMBIT ROM OK — chip=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
               ch, r.chip, r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
    } else {
        printf("ch%d: no ROM response (%s) — check wiring / that Lua is stopped\r\n",
               ch, esp_err_to_name(err));
    }
    return (err == ESP_OK) ? 0 : 1;
}

/* Full ROM flash of an AMBIT from an SD firmware folder (bootloader/partitions/
 * boot_app0/app.bin). Works on bare / bricked / any-firmware units. Preserves NVS
 * calibration (0x9000 untouched). Stop Lua first (`lua stop`); confirm afterward
 * with `ambit_versions`. */
static int cli_cmd_ambit_flash(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        printf("Usage: ambit_flash <0-3> <version|/sdcard/dir> [baud=460800]\r\n");
        printf("  version X  => /sdcard/ambit_fw/X/{bootloader,partitions,boot_app0,app}.bin\r\n");
        printf("  Writes the 4 code regions over the C3 ROM; preserves NVS. Stop Lua first.\r\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    if (ch < 0 || ch >= UART_SENSOR_NUM_CHANNELS) {
        printf("Channel must be 0-%d\r\n", UART_SENSOR_NUM_CHANNELS - 1);
        return 1;
    }
    char dir[160];
    if (argv[2][0] == '/') {
        snprintf(dir, sizeof dir, "%s", argv[2]);
    } else {
        snprintf(dir, sizeof dir, "/sdcard/ambit_fw/%s", argv[2]);
    }
    uint32_t baud = (argc == 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;

    printf("Flashing AMBIT ch%d from %s (this takes ~10-60 s)...\r\n", ch, dir);
    ambit_flash_image_result_t r;
    esp_err_t e = ambit_flash_image((uint8_t)ch, dir, baud, &r);
    if (e == ESP_OK) {
        printf("ch%d: FLASH OK — %d regions, %u bytes; AMBIT reset into new app.\r\n"
               "  Confirm with `ambit_versions`.\r\n",
               ch, r.regions_written, (unsigned)r.total_bytes);
    } else {
        printf("ch%d: FLASH FAILED (%s) after %d region(s) — see log\r\n",
               ch, esp_err_to_name(e), r.regions_written);
    }
    return (e == ESP_OK) ? 0 : 1;
}

/* Detect + report AMBIT firmware drift vs the SD target image (no flashing). Finds
 * the highest /sdcard/ambit_fw/<major.minor.batch>/ with the 4 region files and
 * compares each present AMBIT's running version to it. Read-only; safe with Lua up. */
static int cli_cmd_ambit_check(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: ambit_check\r\n");
        return 1;
    }
    int m = ambit_flash_check();
    if (m < 0) {
        printf("No SD target image — put the build in "
               "/sdcard/ambit_fw/<major.minor.batch>/ (4 region files).\r\n");
    } else if (m == 0) {
        printf("All present AMBITs match the SD target.\r\n");
    } else {
        printf("%d channel(s) differ from the SD target — see the log for the "
               "ambit_flash command(s) to run.\r\n", m);
    }
    return 0;
}

/* Operator control of the Lua measurement script. `exec` runs a snippet in an
 * ephemeral state ALONGSIDE a running main.lua (same env: device/ambit/uart/
 * db/sync) — the console-side twin of the MQTT lua_exec command. */
static int cli_cmd_lua(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: lua <start|stop|status|exec <code...>>\r\n");
        return 1;
    }
    if (strcmp(argv[1], "start") == 0) {
        esp_err_t err = lua_runner_start();
        if (err == ESP_ERR_INVALID_STATE) printf("already running\r\n");
        else if (err != ESP_OK)           printf("start failed: %s\r\n", esp_err_to_name(err));
        else                              printf("started (loads /sdcard/main.lua)\r\n");
        return (err == ESP_OK || err == ESP_ERR_INVALID_STATE) ? 0 : 1;
    }
    if (strcmp(argv[1], "stop") == 0) {
        esp_err_t err = lua_runner_stop(5000);
        if (err == ESP_ERR_TIMEOUT) {
            printf("still busy in a C call (e.g. a long UART read) — it will exit "
                   "when that returns\r\n");
        } else {
            printf("%s\r\n", lua_runner_is_running() ? "stop signaled" : "stopped");
        }
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        printf("lua script: %s\r\n", lua_runner_is_running() ? "RUNNING" : "stopped");
        return 0;
    }
    if (strcmp(argv[1], "exec") == 0) {
        if (argc < 3) {
            printf("Usage: lua exec <code...>   e.g. lua exec return device.uptime_ms()\r\n");
            return 1;
        }
        /* Rejoin argv[2..] — the console splits on spaces. */
        char code[512] = "";
        size_t off = 0;
        for (int i = 2; i < argc && off < sizeof(code) - 1; i++) {
            off += (size_t)snprintf(code + off, sizeof(code) - off, "%s%s",
                                    i > 2 ? " " : "", argv[i]);
        }
        char result[256] = "";
        esp_err_t err = lua_runner_exec(code, 120000, result, sizeof result);
        if (err == ESP_OK) {
            printf("ok: %s\r\n", result[0] ? result : "(no return value)");
            return 0;
        }
        printf("error (%s): %s\r\n", esp_err_to_name(err), result);
        return 1;
    }
    printf("unknown subcommand '%s' (start|stop|status|exec)\r\n", argv[1]);
    return 1;
}

/* Print the firmware version of every present AMBIT (cmd 33/2) — the fleet
 * staleness view. Synchronous (console task); the MQTT `ambit_versions` command
 * does the same sweep on the worker and publishes a JSON report. */
static int cli_cmd_ambit_versions(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("AMBIT firmware versions:\r\n");
    for (uint8_t c = 0; c < UART_SENSOR_NUM_CHANNELS; c++) {
        bool connected = false;
        cmd_result_t pr = cmd_uart_ping(c, &connected);
        if (pr.status != ESP_OK || !connected) {
            printf("  AMBIT%u: absent\r\n", c + 1);
            continue;
        }
        uint8_t buf[64];
        size_t  len = 0;
        cmd_result_t r = cmd_ambit_get_info(c, AMBIT_INFO_FW, buf, sizeof(buf), &len);
        if (r.status == ESP_OK && len >= sizeof(ambit_fw_info_t)) {
            const ambit_fw_info_t *fw = (const ambit_fw_info_t *)buf;
            printf("  AMBIT%u: v%u.%u.%u  (built %.12s)\r\n",
                   c + 1, fw->major, fw->minor, fw->batch, fw->fw_date);
        } else {
            printf("  AMBIT%u: present, no version (%s)\r\n", c + 1, esp_err_to_name(r.status));
        }
    }
    return 0;
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
        .help = "rtc | rtc set <epoch> | rtc set <YYYY-MM-DD> <HH:MM:SS> (UTC)",
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
    static const esp_console_cmd_t record_env_cmd = {
        .command = "record_env",
        .help = "read BME280 and store one T/H/P event in the DB",
        .func = cli_cmd_record_env,
    };
    static const esp_console_cmd_t log_status_cmd = {
        .command = "log_status",
        .help = "show SD log writer state (file size, buffered/dropped bytes)",
        .func = cli_cmd_log_status,
    };
    static const esp_console_cmd_t i2cscan_cmd = {
        .command = "i2cscan",
        .help = "scan the shared I2C bus for responding 7-bit addresses",
        .func = cli_cmd_i2cscan,
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
        .help    = "uart_query <ch> <message> [timeout_ms=1000]  ASCII line query (LF-terminated, never stores)",
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
    static const esp_console_cmd_t ambit_ota_cmd = {
        .command = "ambit_ota",
        .help    = "ambit_ota <0-3|all> <url>  stream a new AMBIT firmware image over UART (OTA)",
        .func    = cli_cmd_ambit_ota,
    };
    static const esp_console_cmd_t ambit_versions_cmd = {
        .command = "ambit_versions",
        .help    = "ambit_versions  print every present AMBIT's firmware version",
        .func    = cli_cmd_ambit_versions,
    };
    static const esp_console_cmd_t ambit_dl_cmd = {
        .command = "ambit_dl",
        .help    = "ambit_dl <0-3> <enter|run>  force AMBIT into ROM download mode / back to app (flasher bench test)",
        .func    = cli_cmd_ambit_dl,
    };
    static const esp_console_cmd_t ambit_probe_cmd = {
        .command = "ambit_probe",
        .help    = "ambit_probe <0-3>  enter ROM download + connect via esp-serial-flasher, read chip+MAC (stop Lua first)",
        .func    = cli_cmd_ambit_probe,
    };
    static const esp_console_cmd_t ambit_flash_cmd = {
        .command = "ambit_flash",
        .help    = "ambit_flash <0-3> <version|/sdcard/dir> [baud]  full ROM flash from SD (4 regions, preserves NVS; stop Lua first)",
        .func    = cli_cmd_ambit_flash,
    };
    static const esp_console_cmd_t ambit_check_cmd = {
        .command = "ambit_check",
        .help    = "ambit_check  report each AMBIT's version vs the SD target /sdcard/ambit_fw/<M.m.b> (no flashing)",
        .func    = cli_cmd_ambit_check,
    };
    static const esp_console_cmd_t lua_cmd = {
        .command = "lua",
        .help    = "lua start|stop|status|exec <code...>  control / poke the Lua script",
        .func    = cli_cmd_lua,
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
    static const esp_console_cmd_t sync_cmd = {
        .command = "sync",
        .help    = "sync [sun|loc|interval|clock|weekly|sunrise|sunset …]  RTC scheduling triggers",
        .func    = cli_cmd_sync,
    };
    static const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help    = "restart the device",
        .func    = cli_cmd_reboot,
    };
    static const esp_console_cmd_t ota_status_cmd = {
        .command = "ota_status",
        .help    = "show running/boot/next OTA partition + rollback state",
        .func    = cli_cmd_ota_status,
    };
    static const esp_console_cmd_t ota_selftest_cmd = {
        .command = "ota_selftest",
        .help    = "clone running image to the other slot + set boot (OTA Stage-1 test)",
        .func    = cli_cmd_ota_selftest,
    };
    static const esp_console_cmd_t ota_mark_valid_cmd = {
        .command = "ota_mark_valid",
        .help    = "esp_ota_mark_app_valid_cancel_rollback() — commit a PENDING_VERIFY image",
        .func    = cli_cmd_ota_mark_valid,
    };
#ifdef CONFIG_HEAP_TRACING_STANDALONE
    static const esp_console_cmd_t heaptrace_cmd = {
        .command = "heaptrace",
        .help    = "heaptrace <start|stop|dump>  diagnostic heap-leak trace",
        .func    = cli_cmd_heaptrace,
    };
#endif

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

    err = esp_console_cmd_register(&record_env_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&log_status_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&i2cscan_cmd);
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

    err = esp_console_cmd_register(&ambit_ota_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_versions_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_dl_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_probe_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_flash_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ambit_check_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&lua_cmd);
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

    err = esp_console_cmd_register(&sync_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&reboot_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ota_status_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ota_selftest_cmd);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_console_cmd_register(&ota_mark_valid_cmd);
    if (err != ESP_OK) {
        return err;
    }

#ifdef CONFIG_HEAP_TRACING_STANDALONE
    err = esp_console_cmd_register(&heaptrace_cmd);
    if (err != ESP_OK) {
        return err;
    }
#endif

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

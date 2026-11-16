"""Production MP2731 input-current-limit driver + CLI/MQTT handlers, with I2C/worker
boundary fakes (docs/charger-iin-limit.md). Mirrors tests/test_vin_limit.py."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Keep the fakes at hardware/scheduling boundaries. The actual driver, CLI
# parser, asynchronous MQTT handler and pinned SDK's cJSON execute under ASan/UBSan.
STUBS = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_ERR_NOT_ALLOWED 0x10d
const char *esp_err_to_name(esp_err_t err);
""",
    "esp_log.h": "#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
    "freertos/FreeRTOS.h": """#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xffffffffu
typedef struct { int held; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void h_critical_enter(portMUX_TYPE *m);
void h_critical_exit(portMUX_TYPE *m);
#define portENTER_CRITICAL(m) h_critical_enter(m)
#define portEXIT_CRITICAL(m) h_critical_exit(m)
""",
    "freertos/task.h": "#include \"FreeRTOS.h\"\nvoid vTaskDelay(TickType_t ticks);\n",
    "driver/i2c.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int i2c_port_t;
#define I2C_NUM_MAX 2
esp_err_t i2c_master_write_read_device(i2c_port_t port, uint8_t addr,
    const uint8_t *reg, size_t reg_size, uint8_t *out, size_t size, TickType_t timeout);
esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t addr,
    const uint8_t *buf, size_t size, TickType_t timeout);
""",
    "i2c_bus.h": """#include "driver/i2c.h"
esp_err_t i2c_bus_get_port(i2c_port_t *port);
esp_err_t i2c_bus_lock(TickType_t timeout);
esp_err_t i2c_bus_unlock(void);
esp_err_t i2c_bus_probe_addr(i2c_port_t port, uint8_t addr);
""",
    "device_commands.h": """#pragma once
#include "sensing_port.h"
typedef struct { esp_err_t status; char message[256]; } cmd_result_t;
cmd_result_t cmd_get_iin_limit(power_iin_limit_t *out);
cmd_result_t cmd_set_iin_limit(uint16_t ma, power_iin_limit_t *out);
""",
}


class IinLimitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="iin-limit-")
        directory = Path(cls.temp.name)
        cls.binary = directory / "iin-limit"
        for name, content in STUBS.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
        sdk = packages / "framework-espidf@3.50500.0"
        if not sdk.exists():
            sdk = packages / "framework-espidf"
        cjson = sdk / "components/json/cJSON"
        if not (cjson / "cJSON.c").exists():
            raise RuntimeError("Pinned ESP-IDF cJSON is required for IIN-limit qualification")
        subprocess.run([
            os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
            f"-I{directory}", f"-I{ROOT / 'tests/mp2731_driver_stubs'}", f"-I{cjson}",
            *(f"-I{ROOT / 'components' / name / 'include'}" for name in
              ("domain", "mp2731", "CLI", "command_router")),
            str(ROOT / "tests/iin_limit_host.c"),
            str(ROOT / "components/mp2731/mp2731.c"),
            str(ROOT / "components/mp2731/mp2731_core.c"),
            str(ROOT / "components/mp2731/mp2731_diagnostics.c"),
            str(ROOT / "components/mp2731/mp2731_redetect.c"),
            str(ROOT / "components/CLI/cli_iin_limit.c"),
            str(ROOT / "components/command_router/iin_limit_command.c"),
            str(cjson / "cJSON.c"), "-lm", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_every_setting_preserves_hiz_and_en_ilim_and_every_byte_decodes(self):
        self.run_case("limits")

    def test_absent_charger_null_output_and_invalid_values_never_touch_the_bus(self):
        self.run_case("invalid")

    def test_bus_failures_never_write_blind_and_never_report_success(self):
        self.run_case("failures")

    def test_cli_reads_sets_and_rejects_invalid_syntax(self):
        out = self.run_case("cli")
        self.assertIn("REG00=0x52", out)
        self.assertIn("expected 100..3250 mA in 50 mA steps", out)

    def test_mqtt_copies_and_defers_request_then_returns_hardware_readback(self):
        self.run_case("mqtt")

    def test_mqtt_rejects_bad_values_and_ids_without_dispatch(self):
        self.run_case("mqtt_invalid")

    def test_busy_queue_failure_and_io_error_are_retryable(self):
        self.run_case("mqtt_failures")

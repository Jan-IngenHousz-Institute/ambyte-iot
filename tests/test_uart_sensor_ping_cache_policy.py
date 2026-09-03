import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class UartSensorPingCachePolicyTest(unittest.TestCase):
    def test_host_policy(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / "uart_sensor_ping_cache_policy_host"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "components" / "uart_sensors"),
                    str(ROOT / "tests" / "uart_sensor_ping_cache_policy_host.c"),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                check=True,
            )
            subprocess.run([str(binary)], cwd=ROOT, check=True)

    def test_shared_app_resets_open_a_negative_cache_grace_window(self) -> None:
        source = (ROOT / "components" / "uart_sensors" / "uart_sensors.c").read_text(
            encoding="utf-8"
        )
        reset_helper = source.split(
            "static void ambit_note_shared_app_reset", 1
        )[1].split("static inline bool deadline_reached", 1)[0]
        self.assertIn(
            "s_ping_fail_cache_not_before_us = now_us() + AMBIT_APP_BOOT_GRACE_US;",
            reset_helper,
        )
        self.assertIn("s_ch[channel].ping_ts = 0;", reset_helper)

        boot_reset = source.split("static esp_err_t ambit_boot_gpio_init", 1)[1].split(
            "esp_err_t uart_sensors_flash_session_begin", 1
        )[0]
        run_app = source.split("esp_err_t uart_sensors_run_app", 1)[1].split(
            "/* ── Ambit wake sequence", 1
        )[0]
        self.assertIn("ambit_note_shared_app_reset();", boot_reset)
        self.assertIn("ambit_note_shared_app_reset();", run_app)

        ping = source.split("static esp_err_t do_ping", 1)[1].split(
            "/* ── Public: status", 1
        )[0]
        self.assertIn("uart_sensor_ping_result_cacheable(", ping)


if __name__ == "__main__":
    unittest.main()

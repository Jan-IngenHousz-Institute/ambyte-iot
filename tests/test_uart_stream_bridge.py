"""Executable host and integration-contract checks for the UART stream bridge."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class UartStreamBridgeTest(unittest.TestCase):
    def test_production_scanner_sink_deadline_and_json_helpers(self):
        platformio_home = Path(
            os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")
        )
        cjson_dir = (
            platformio_home
            / "packages/framework-espidf/components/json/cJSON"
        )
        cjson_source = cjson_dir / "cJSON.c"
        self.assertTrue(
            cjson_source.is_file(),
            "ESP-IDF cJSON missing; run the normal PlatformIO dependency bootstrap",
        )

        with tempfile.TemporaryDirectory(prefix="uart-stream-host-") as tmp:
            binary = Path(tmp) / "uart_stream_bridge_host"
            compile_cmd = [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'components/domain/include'}",
                f"-I{ROOT / 'components/CLI/include'}",
                f"-I{cjson_dir}",
                str(ROOT / "tests/uart_stream_bridge_host.c"),
                str(ROOT / "components/domain/uart_stream_support.c"),
                str(ROOT / "components/CLI/uart_stream_cli_support.c"),
                str(cjson_source),
                "-lm",
                "-o",
                str(binary),
            ]
            subprocess.run(compile_cmd, check=True, cwd=ROOT)
            result = subprocess.run(
                [str(binary)], check=True, cwd=ROOT, capture_output=True, text=True
            )
            self.assertIn("production helper tests: ok", result.stdout)

    def test_invalid_json_is_rejected_before_begin_or_uart_call(self):
        source = (ROOT / "components/CLI/CLI.c").read_text(encoding="utf-8")
        handler = source.split("static int cli_cmd_uart_stream_query(", 1)[1].split(
            "static int cli_cmd_ambit_temp(", 1
        )[0]
        validation = handler.index("uart_stream_json_request_valid")
        begin = handler.index("UART_STREAM_BEGIN")
        call = handler.index("cmd_uart_stream_query")
        self.assertLess(validation, begin)
        self.assertLess(validation, call)

    def test_existing_buffered_query_and_nonstoring_bridge_are_additive(self):
        cli_source = (ROOT / "components/CLI/CLI.c").read_text(encoding="utf-8")
        legacy = cli_source.split("static int cli_cmd_uart_query(", 1)[1].split(
            "static int cli_cmd_uart_stream_query(", 1
        )[0]
        stream = cli_source.split("static int cli_cmd_uart_stream_query(", 1)[1].split(
            "static int cli_cmd_ambit_temp(", 1
        )[0]
        device_source = (
            ROOT / "components/device_commands/device_commands.c"
        ).read_text(encoding="utf-8")
        device_stream = device_source.split("cmd_result_t cmd_uart_stream_query(", 1)[
            1
        ].split("void device_commands_on_mqtt_disconnect", 1)[0]

        self.assertIn("char   resp[256];", legacy)
        self.assertIn("cmd_uart_text_query", legacy)
        self.assertNotIn("resp[256]", stream)
        self.assertIn("UART_STREAM_BEGIN", stream)
        self.assertIn("UART_STREAM_DATA offset=%u hex=", cli_source)
        self.assertIn("UART_STREAM_END", stream)
        self.assertIn("UART_STREAM_ERROR", stream)
        self.assertLess(
            device_stream.index("sensor_transaction_begin()"),
            device_stream.rindex("s_cfg.uart_stream_query"),
        )
        self.assertLess(
            device_stream.rindex("s_cfg.uart_stream_query"),
            device_stream.index("sensor_transaction_end()"),
        )
        self.assertNotIn("store_event", device_stream)
        self.assertNotIn("mqtt", device_stream.lower())


if __name__ == "__main__":
    unittest.main()

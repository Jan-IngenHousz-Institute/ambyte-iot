# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the entire production UART adapter; only SDK hardware calls fake."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SensorsUartTest(unittest.TestCase):
    def test_raw_transport_failures_next_block_and_real_origin_counter(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "uart"
            includes = [ROOT / "tests/sensors_host/include", ROOT / "tests/identity_host/include"]
            includes += list((ROOT / "components").glob("*/include"))
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                "-DportTICK_PERIOD_MS=1", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *(f"-I{p}" for p in includes),
                str(ROOT / "components/uart_sensors/uart_sensors.c"),
                str(ROOT / "components/uart_stream/uart_stream_support.c"),
                str(ROOT / "components/sensor_inventory/sensor_inventory.c"),
                str(ROOT / "tests/sensors_host/uart_driver.c"), "-pthread", "-o", str(binary)], check=True)
            result = subprocess.run([binary], capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn(b"shared_bank_race=1", result.stdout)


if __name__ == "__main__":
    unittest.main()

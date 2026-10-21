# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual console output holds stdout for a bounded deadline, even with no TX."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SensorsConsoleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "console"
        includes = [ROOT / "tests/svc_host/include", ROOT / "tests/identity_host/include"]
        includes += list((ROOT / "components").glob("*/include"))
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", *(f"-I{p}" for p in includes),
            str(ROOT / "components/sensor_inventory/sensor_inventory.c"),
            str(ROOT / "components/sensor_inventory/sensor_json.c"), str(ROOT / "components/CLI/cli_sensors.c"),
            str(ROOT / "tests/sensors_host/console_driver.c"), "-Wl,--wrap=fwrite", "-Wl,--wrap=fputs",
            "-Wl,--wrap=fflush", "-pthread", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_full_extreme_four_port_line_and_stalled_reader_release(self):
        for case in ("full", "stall"):
            with self.subTest(case=case):
                p = subprocess.run([self.binary, case], capture_output=True, timeout=15)
                self.assertEqual(p.returncode, 0, p.stderr.decode())
                if case == "full":
                    self.assertLessEqual(len(p.stdout), 12307)
                    self.assertEqual(len(p.stdout.splitlines()), 3)  # separator, JSON, extra CR
                    obj = json.loads(p.stdout.strip())
                    self.assertEqual(len(obj["ports"]), 4)
                    self.assertLess(obj["ports"][0]["calibration"]["value"]["temp_offset"], -3e38)
                else:
                    self.assertIn(b"lock_released=1", p.stdout)


if __name__ == "__main__":
    unittest.main()

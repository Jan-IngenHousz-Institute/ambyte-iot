# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The live-location scheduler re-arms after an unauthorized timer firing.

Builds tests/location_aws_timer_host.c, which includes the production
components/command_router/location_aws.c with its RTOS, timer and MQTT
boundaries replaced (review of #100: live refresh used to die for the rest of
the boot once a timer fired while offline).
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class LocationAwsTimerTest(unittest.TestCase):
    def test_rearms_on_grant_without_reconnect_bursts(self):
        with tempfile.TemporaryDirectory(prefix="loc-timer-") as tmp:
            binary = Path(tmp) / "loc-timer"
            subprocess.run([
                os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                f"-I{ROOT / 'tests/location_aws_timer_host'}", f"-I{ROOT / 'tests/host_stubs'}",
                f"-I{ROOT / 'components/domain/include'}",
                f"-I{ROOT / 'components/command_router/include'}",
                f"-I{ROOT / 'components/device_config/include'}",
                str(ROOT / "tests/location_aws_timer_host.c"), "-o", str(binary),
            ], check=True, cwd=ROOT / "tests")
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("LOCATION_AWS_TIMER_OK", run.stdout)


if __name__ == "__main__":
    unittest.main()

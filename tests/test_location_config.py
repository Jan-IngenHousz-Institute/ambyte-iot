# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Executable and source contracts for atomic schedule site configuration."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.site_state_blob import decode_site_state, encode_site_state


ROOT = Path(__file__).resolve().parents[1]
DEVICE_CONFIG = (ROOT / "components/device_config/device_config.c").read_text()
DEVICE_CONFIG_H = (
    ROOT / "components/device_config/include/device_config.h"
).read_text()
COMMAND_ROUTER = (
    ROOT / "components/command_router/command_router.c"
).read_text()
CLI = (ROOT / "components/CLI/CLI.c").read_text()
RUNNER_ACTIONS = (
    ROOT / "components/sched_runner/sched_runner_actions.c"
).read_text()


class LocationConfigContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="device-config-site-host-")
        cls.host = Path(cls.tmp.name) / "device_config_site_state_host"
        subprocess.run(
            [
                os.environ.get("CC", shutil.which("clang") or "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tests/host_stubs'}",
                f"-I{ROOT / 'components/device_config/include'}",
                f"-I{ROOT / 'components/timezone/include'}",
                str(ROOT / "tests/device_config_site_state_host.c"),
                str(ROOT / "components/device_config/device_config.c"),
                "-lm",
                "-o",
                str(cls.host),
            ],
            check=True,
            cwd=ROOT,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_device_config_exposes_atomic_bounded_persistent_writers(self):
        for name in ("lat", "lon", "deployment"):
            self.assertIn(f"device_config_set_{name}", DEVICE_CONFIG_H)
            self.assertIn(f"device_config_set_{name}", DEVICE_CONFIG)
        self.assertIn("device_config_set_location", DEVICE_CONFIG_H)
        grouped = DEVICE_CONFIG.split("esp_err_t device_config_set_location(", 1)[1]
        self.assertIn("site_state_write(&state)", grouped)
        writer = DEVICE_CONFIG.split("static esp_err_t site_state_write(", 1)[1]
        writer = writer.split("\n}\n", 1)[0]
        self.assertEqual(writer.count("nvs_set_blob"), 1)
        self.assertEqual(writer.count("nvs_commit"), 1)
        self.assertIn('KEY_SITE_STATE      "site_state"', DEVICE_CONFIG)
        self.assertIn("val < -90.0 || val > 90.0", DEVICE_CONFIG)
        self.assertIn("val < -180.0 || val > 180.0", DEVICE_CONFIG)

    def test_real_device_config_fault_injection(self):
        result = subprocess.run(
            [str(self.host)], check=True, cwd=ROOT, capture_output=True, text=True
        )
        self.assertIn("site_state fault tests: OK", result.stdout)

    def test_python_provisioners_match_firmware_blob_bytes(self):
        result = subprocess.run(
            [str(self.host), "--dump"],
            check=True,
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        firmware_blob = bytes.fromhex(result.stdout.strip())
        python_blob = encode_site_state(52.173, 5.819, "greenhouse-a")
        self.assertEqual(firmware_blob, python_blob)
        decoded = decode_site_state(firmware_blob)
        self.assertEqual(decoded.lat, 52.173)
        self.assertEqual(decoded.lon, 5.819)
        self.assertEqual(decoded.deployment, "greenhouse-a")

    def test_mqtt_command_persists_then_applies_and_replies(self):
        branch = COMMAND_ROUTER.split('strcmp(type, "set_location") == 0', 1)[1]
        branch = branch.split('strcmp(type, "set_time") == 0', 1)[0]
        for call in (
            "device_config_set_location",
            "time_sync_set_location",
        ):
            self.assertIn(call, branch)
        self.assertIn('"set_location_result"', branch)
        self.assertIn('"deployment"', branch)

    def test_console_sync_loc_persists_before_applying(self):
        branch = CLI.split('strcmp(sub, "loc") == 0', 1)[1]
        branch = branch.split('strcmp(sub, "interval") == 0', 1)[0]
        self.assertLess(branch.index("device_config_set_location"),
                        branch.index("time_sync_set_location"))
        self.assertIn("device_config_get_deployment", branch)
        self.assertIn("deployment=%s", branch)

    def test_deployment_placeholder_reads_persisted_value_at_action_time(self):
        placeholder = RUNNER_ACTIONS.split(
            'strcmp(ph, "$deployment") == 0', 1
        )[1].split('strcmp(ph, "$lat") == 0', 1)[0]
        self.assertIn("device_config_get_deployment", placeholder)


if __name__ == "__main__":
    unittest.main()

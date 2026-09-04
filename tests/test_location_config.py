# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Source-contract coverage for persistent schedule location configuration."""

import unittest
from pathlib import Path


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
    def test_device_config_exposes_bounded_persistent_writers(self):
        for name in ("lat", "lon", "deployment"):
            self.assertIn(f"device_config_set_{name}", DEVICE_CONFIG_H)
            self.assertIn(f"device_config_set_{name}", DEVICE_CONFIG)
        self.assertIn("nvs_set_blob(s_handle, key, &val, sizeof(val))", DEVICE_CONFIG)
        self.assertIn("device_config_set_location", DEVICE_CONFIG_H)
        grouped = DEVICE_CONFIG.split("esp_err_t device_config_set_location(", 1)[1]
        self.assertIn("cfg_restore_double(KEY_LAT", grouped)
        self.assertIn("cfg_restore_double(KEY_LON", grouped)
        self.assertIn("cfg_restore_string(KEY_DEPLOYMENT", grouped)
        self.assertIn("val < -90.0 || val > 90.0", DEVICE_CONFIG)
        self.assertIn("val < -180.0 || val > 180.0", DEVICE_CONFIG)

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

    def test_deployment_placeholder_reads_persisted_value_at_action_time(self):
        placeholder = RUNNER_ACTIONS.split(
            'strcmp(ph, "$deployment") == 0', 1
        )[1].split('strcmp(ph, "$lat") == 0', 1)[0]
        self.assertIn("device_config_get_deployment", placeholder)


if __name__ == "__main__":
    unittest.main()

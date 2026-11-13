# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Timezone from location: the production policy end to end on the host.

tests/timezone_policy_host.c links the real timezone_command.c, device_location.c,
device_config.c (over a write-counting fake NVS), timezone.c, tz_geo.c with the
checked-in map, and pinned-IDF cJSON. Fixtures include the live AWS IP-solver
answers measured on 2026-11-13: Uluru's cellular egress resolved to Adelaide
(-34.8545, 138.6471, +-50 km); a Dutch IP estimate (+-5 km) is represented by a
synthetic Amsterdam point (the measured one located the bench site).
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SCENARIOS = [
    "boot_without_location",
    "upgrade_from_2_16_berlin",
    "upgrade_from_2_16_darwin",
    "set_location_applies",
    "plain_set_is_a_fallback",
    "pin_wins_until_location_changes",
    "auto_releases_pin",
    "live_beats_preset_from_another_country",
    "configured_beats_live_in_same_country",
    "live_without_configured_applies_at_once",
    "office_ip_estimate",
    "live_zone_change_needs_confirmation",
    "unusable_estimates_ignored",
    "persist_failure_reports_and_keeps_zone",
    "solar_follows_live",
    "solar_ignores_live_noise",
    "solar_configured_and_default",
    "loc_live_off_drops_live",
    "pin_keeps_solar_location",
    "ip_never_overrides_configured",
    "ip_needs_two_agreeing_an_hour_apart",
    "expired_fix_is_reported",
    "readers_never_wait_on_flash",
]


class TimezonePolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
        sdk = packages / "framework-espidf@3.50500.0"
        if not sdk.exists():
            sdk = packages / "framework-espidf"
        cjson = sdk / "components/json/cJSON"
        if not (cjson / "cJSON.c").exists():
            raise RuntimeError("Pinned ESP-IDF cJSON is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="tz-policy-")
        out = Path(cls.temp.name)
        (out / "esp_log.h").write_text("#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n"
                                       "#define ESP_LOGW(...) ((void)0)\n")
        cls.binary = out / "tz-policy"
        comps = ROOT / "components"
        subprocess.run([
            os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-g", "-pthread", "-Wl,--wrap=time",
            f"-I{out}", f"-I{ROOT / 'tests/host_stubs'}", f"-I{cjson}",
            *(f"-I{comps / name / 'include'}" for name in
              ("command_router", "timezone", "time_sync", "device_config")),
            str(ROOT / "tests/timezone_policy_host.c"),
            str(comps / "command_router/timezone_command.c"),
            str(comps / "command_router/device_location.c"),
            str(comps / "device_config/device_config.c"),
            str(comps / "timezone/timezone.c"), str(comps / "timezone/tz_geo.c"),
            str(comps / "time_sync/time_sync.c"), str(cjson / "cJSON.c"), "-lm", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_scenarios(self):
        for scenario in SCENARIOS:
            with self.subTest(scenario):
                run = subprocess.run([str(self.binary), str(ROOT / "components/timezone/tz_geo.bin"), scenario],
                                     capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stderr[-3000:])


if __name__ == "__main__":
    unittest.main()

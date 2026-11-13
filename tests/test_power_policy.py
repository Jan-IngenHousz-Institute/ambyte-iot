# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""MP2731 power read + power policy (2026-11-13 Uluru incident).

Compiles the production components/mp2731/mp2731_core.c and
components/device_commands/power_policy.c against a fake charger
(tests/power_policy_host.c) and runs every scenario. The same harness built with
-DLEGACY_IMPL replays the incident scenarios against the v2.16.0 read and gate
(tests/fixtures/power_v2_16_0/legacy_power.c): those MUST fail - the red half
of the red/green reproduction. The replays are MODELED: the field never
captured full REG0C/REG0D, so the all-zero replay assumes status 0x20 /
fault 0x00 and the frozen-frame replay assumes status 0x01 / INPUT_FAULT. Source-contract tests pin the wiring the host
build cannot see (gate, SD guard, AMBIT flash gate, telemetry)."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/power_policy_host.c"

# Scenarios that encode a field failure of v2.16.0: red on the legacy code.
INCIDENT_SCENARIOS = {
    "incident_allzero": "held the publish gate CLOSED",
    "incident_frozen_frame_flash": "let an AMBIT flash proceed without external power",
    "stuck_start_bit_needs_edge": "must still produce a conversion",
}


def build(out: Path, legacy: bool) -> Path:
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or "cc"
    binary = out / ("legacy" if legacy else "current")
    cmd = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           f"-I{ROOT / 'tests/host_stubs'}",
           f"-I{ROOT / 'components/domain/include'}",
           f"-I{ROOT / 'components/mp2731/include'}",
           f"-I{ROOT / 'components/device_commands/include'}",
           str(HOST),
           str(ROOT / "components/mp2731/mp2731_core.c"),
           str(ROOT / "components/device_commands/power_policy.c"),
           "-o", str(binary)]
    if legacy:
        cmd[1:1] = ["-DLEGACY_IMPL"]
        cmd.insert(-2, str(ROOT / "tests/fixtures/power_v2_16_0/legacy_power.c"))
    if "clang" in Path(cc).name:
        cmd[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return binary


class PowerPolicyHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.current = build(Path(cls.tmp.name), legacy=False)
        cls.legacy = build(Path(cls.tmp.name), legacy=True)
        listing = subprocess.run([str(cls.current), "--list"], check=True,
                                 capture_output=True, text=True).stdout.split()
        cls.scenarios = listing

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_every_scenario_passes_on_the_fix(self):
        self.assertGreaterEqual(len(self.scenarios), 19)
        for name in self.scenarios:
            with self.subTest(scenario=name):
                r = subprocess.run([str(self.current), name], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn(f"PASS {name}", r.stdout)

    def test_incident_scenarios_are_red_on_v2_16_0(self):
        """The fix is load-bearing: v2.16.0's own read + gate fail the replays."""
        for name, symptom in INCIDENT_SCENARIOS.items():
            with self.subTest(scenario=name):
                r = subprocess.run([str(self.legacy), name], capture_output=True, text=True)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(symptom, r.stderr)


class PowerWiringContractTest(unittest.TestCase):
    """What the host build cannot link: the production call sites."""

    def read(self, rel):
        return (ROOT / rel).read_text()

    def test_publish_gate_never_closes(self):
        src = self.read("components/device_commands/device_commands.c")
        body = src[src.index("bool device_commands_publish_power_ok(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("return power_publish_ok(&s_pub_gate);", body)
        self.assertNotIn("return false", body)
        policy = self.read("components/device_commands/power_policy.c")
        fn = policy[policy.index("bool power_publish_ok(const power_gate_t *g)"):]
        fn = fn[:fn.index("\n}\n")]
        self.assertIn("return true;", fn)
        self.assertNotIn("return false", fn)

    def test_power_cache_uses_the_monotonic_clock(self):
        src = self.read("components/device_commands/device_commands.c")
        body = src[src.index("static void power_state_refresh(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("const int64_t now = mono_ms();", body)
        self.assertIn("power_refresh_due(s_pub_gate_eval_ms, now, PUB_GATE_EVAL_INTERVAL_MS)", body)
        self.assertNotIn("now_ms()", body)

    def test_flash_gate_refuses_without_a_charger_monitor(self):
        src = self.read("components/device_commands/device_commands.c")
        body = src[src.index("bool device_commands_flash_power_ok(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("return AMBYTE_FLASH_WITHOUT_CHARGER != 0;", body)
        self.assertIn("#define AMBYTE_FLASH_WITHOUT_CHARGER 0", src)
        self.assertIn("return power_gate_flash_ok(&s_pub_gate);", body)

    def test_ambit_boot_sync_is_fail_closed(self):
        src = self.read("components/ambit_flash/ambit_flash.c")
        self.assertNotIn("device_commands_publish_power_ok", src)
        self.assertEqual(src.count("device_commands_flash_power_ok()"), 3)
        ota = self.read("components/ambit_ota/ambit_ota.c")
        self.assertIn("const bool power = device_commands_flash_power_ok();", ota)
        self.assertNotIn("ambit_attach_power_ok", ota)

    def test_sd_guard_uses_the_policy(self):
        src = self.read("main/app_main.c")
        task = src[src.index("static void app_power_guard_task(void *arg)"):]
        task = task[:task.index("\nstatic void app_start_power_guard")]
        self.assertIn("power_guard_step(&guard, reason, &p, app_maintenance_active())", task)
        self.assertIn("POWER_GUARD_UNPARK_UNVERIFIED", task)
        self.assertNotIn("p.battery_mv < ", task)   # thresholds live in the policy

    def test_rejected_frames_never_published_as_measurements(self):
        src = self.read("components/device_commands/device_commands.c")
        self.assertIn("memset(&out->power, 0, sizeof out->power);", src)
        self.assertIn(".power_reason = s_cfg.read_power != NULL ? power_read_reason_name(", src)
        self.assertIn(".publish_gate_reason = s.publish_gate_reason,", src)

    def test_reason_names_are_stable(self):
        port = self.read("components/domain/include/sensing_port.h")
        for name in ("ok", "adc_timeout", "implausible", "i2c_error", "unavailable", "no_input"):
            self.assertIn(f'return "{name}";', port)
        policy = self.read("components/device_commands/power_policy.c")
        for name in ("starting", "external_power", "on_battery", "power_unverified", "no_charger"):
            self.assertIn(f'return "{name}";', policy)

    def test_driver_serialises_whole_reads(self):
        src = self.read("components/mp2731/mp2731.c")
        body = src[src.index("esp_err_t mp2731_read_power(power_reading_t *out)"):]
        body = body[:body.index("\n}\n")]
        self.assertRegex(body, re.compile(r"xSemaphoreTake\(s_read_mtx.*mp2731_core_read.*xSemaphoreGive",
                                          re.S))


if __name__ == "__main__":
    unittest.main()

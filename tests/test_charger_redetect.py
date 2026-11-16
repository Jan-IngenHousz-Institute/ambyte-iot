"""Charger input re-detection (docs/charger-redetect.md).

Policy: the production components/mp2731/mp2731_redetect.c driven by
tests/charger_redetect_policy_host.c, including a modeled replay of the
17 Nov 2026 Uluru stuck state. Driver/frontends: the production MP2731
adapter (included as one translation unit so the reset guard can be set up),
core pulse logic, CLI parser and asynchronous MQTT handler against a fake
charger at the I2C boundary (tests/charger_redetect_host.c). Everything runs
under ASan/UBSan; the pinned SDK's cJSON is the real one."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

DEVICE_COMMANDS_STUB = """#pragma once
#include "sensing_port.h"
typedef struct { esp_err_t status; char message[256]; } cmd_result_t;
cmd_result_t cmd_charger_redetect(power_redetect_method_t method, power_redetect_report_t *out);
cmd_result_t cmd_charger_redetect_auto(int set, power_redetect_auto_t *out);
"""


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or "cc"


def cjson_dir():
    packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
    sdk = packages / "framework-espidf@3.50500.0"
    if not sdk.exists():
        sdk = packages / "framework-espidf"
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").exists():
        raise RuntimeError("Pinned ESP-IDF cJSON is required for charger re-detect qualification")
    return cjson


FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-g"]


class PolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="redetect-policy-")
        cls.binary = Path(cls.tmp.name) / "policy"
        subprocess.run([compiler(), *FLAGS,
                        f"-I{ROOT / 'tests/host_stubs'}",
                        f"-I{ROOT / 'components/domain/include'}",
                        f"-I{ROOT / 'components/mp2731/include'}",
                        str(ROOT / "tests/charger_redetect_policy_host.c"),
                        str(ROOT / "components/mp2731/mp2731_redetect.c"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result.stdout

    def test_field_stuck_state_of_17_nov_triggers_after_the_dwell(self):
        self.run_case("field")

    def test_charging_done_usb_twilight_full_faults_and_frozen_frames_never_trigger(self):
        self.run_case("never")

    def test_dwell_needs_continuous_evidence(self):
        self.run_case("continuity")

    def test_backoff_10_20_40_60_and_per_boot_cap(self):
        self.run_case("backoff")

    def test_collapsing_panel_cannot_rearm_a_fast_loop(self):
        self.run_case("collapse")

    def test_only_sustained_charging_resets_the_backoff(self):
        self.run_case("sustain")

    def test_outcomes_and_pulse_failures(self):
        self.run_case("outcomes")

    def test_precheck_refuses_hiz_disabled_charge_safety_timer_and_vin_limit(self):
        self.run_case("precheck")

    def test_maintenance_veto_operator_switch_and_manual_pulse(self):
        self.run_case("veto")

    def test_render_is_valid_json_and_worst_case_fits(self):
        out = self.run_case("render")
        records = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        fresh = json.loads(records["RENDER_FRESH"])
        self.assertEqual(fresh["auto"], True)
        self.assertEqual(fresh["state"], "idle")
        self.assertIsNone(fresh["last"])
        self.assertIsNone(fresh["disabled_reason"])
        pending = json.loads(records["RENDER_PENDING"])
        self.assertEqual(pending["state"], "pending")
        self.assertEqual(pending["attempts"], 1)
        self.assertEqual(pending["last"]["outcome"], "pending")
        self.assertEqual(pending["last"]["vin_mv"], 7500)
        worst = json.loads(records["RENDER_WORST"])
        self.assertEqual(worst["disabled_reason"], "reset_during_attempt")
        self.assertLess(len(records["RENDER_WORST"]), 512)


class DriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="redetect-driver-")
        directory = Path(cls.tmp.name)
        (directory / "device_commands.h").write_text(DEVICE_COMMANDS_STUB)
        cls.binary = directory / "driver"
        cjson = cjson_dir()
        subprocess.run([compiler(), *FLAGS, "-DESP_ERR_TIMEOUT=0x107",
                        f"-I{directory}", f"-I{ROOT / 'tests/mp2731_driver_stubs'}",
                        f"-I{ROOT / 'tests/host_stubs'}", f"-I{cjson}",
                        *(f"-I{ROOT / 'components' / name / 'include'}" for name in
                          ("domain", "mp2731", "CLI", "command_router")),
                        str(ROOT / "tests/charger_redetect_host.c"),
                        str(ROOT / "components/mp2731/mp2731_core.c"),
                        str(ROOT / "components/mp2731/mp2731_diagnostics.c"),
                        str(ROOT / "components/mp2731/mp2731_redetect.c"),
                        str(ROOT / "components/CLI/cli_charger_redetect.c"),
                        str(ROOT / "components/command_router/charger_redetect_command.c"),
                        str(cjson / "cJSON.c"), "-lm", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result.stdout

    def test_bg_en_pulse_preserves_reserved_bits_and_holds_the_bus(self):
        self.run_case("bg_en")

    def test_bg_en_that_never_latches_is_a_failure(self):
        self.run_case("not_latched")

    def test_stuck_bg_en_is_cleared_on_later_reads(self):
        self.run_case("stuck")

    def test_bus_errors_never_leave_bg_en_set(self):
        self.run_case("bus")

    def test_dpdm_self_clear_and_restore(self):
        self.run_case("dpdm")

    def test_maintenance_lock_refuses_manual_pulses(self):
        self.run_case("veto")

    def test_auto_redetect_end_to_end_through_power_reads(self):
        out = self.run_case("auto")
        line = next(l for l in out.splitlines() if l.startswith("CHARGER="))
        charger = json.loads(line.split("=", 1)[1])
        self.assertEqual(charger["redetect"]["attempts"], 1)
        self.assertEqual(charger["redetect"]["charging"], 1)
        self.assertEqual(charger["redetect"]["last"]["outcome"], "charging")
        self.assertEqual(charger["read_mask"], 0xFFFFFF)

    def test_auto_precheck_never_defeats_an_expired_safety_timer(self):
        self.run_case("safety_timer")

    def test_reset_during_an_attempt_disables_auto_for_the_boot(self):
        self.run_case("reset_guard")

    def test_cli_parses_methods_and_auto_switch(self):
        out = self.run_case("cli")
        self.assertIn("REG0C=0x00", out)
        self.assertIn("+5s", out)

    def test_mqtt_defers_to_worker_and_reports_snapshots(self):
        out = self.run_case("mqtt")
        reply = json.loads(next(l for l in out.splitlines() if l.startswith("REPLY=")).split("=", 1)[1])
        self.assertEqual(reply["type"], "charger_redetect_result")
        self.assertTrue(reply["ok"])
        self.assertEqual(len(reply["snapshots"]), 4)
        self.assertTrue(all(len(s["regs"]) == 48 for s in reply["snapshots"]))
        self.assertLess(len(json.dumps(reply)), 1400)

    def test_mqtt_rejects_bad_input_and_busy(self):
        self.run_case("mqtt_invalid")


if __name__ == "__main__":
    unittest.main()

"""Charger AICO switch and hourly re-arm (docs/charger-aico.md).

Policy: the production components/mp2731/mp2731_aico.c driven by
tests/aico_policy_host.c. Driver/frontends: the production MP2731 adapter
(included as one translation unit), the AICO policy, CLI parser and
asynchronous MQTT handler against a fake charger that models AICO at the I2C
boundary (tests/aico_host.c). The fake asserts that REG00 is only ever written
back exactly as read and REG02 only in bit 0, each under the bus hold of its
own read, and never inside a BG_EN pulse. Everything runs under ASan/UBSan;
the pinned SDK's cJSON is the real one."""
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
cmd_result_t cmd_get_aico(power_aico_t *out);
cmd_result_t cmd_set_aico(bool enabled, power_aico_t *out);
cmd_result_t cmd_reset_aico(power_aico_t *out);
"""

FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-g"]


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or "cc"


def cjson_dir():
    packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
    sdk = packages / "framework-espidf@3.50500.0"
    if not sdk.exists():
        sdk = packages / "framework-espidf"
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").exists():
        raise RuntimeError("Pinned ESP-IDF cJSON is required for AICO qualification")
    return cjson


def records(out):
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


class PolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="aico-policy-")
        cls.binary = Path(cls.tmp.name) / "policy"
        subprocess.run([compiler(), *FLAGS,
                        f"-I{ROOT / 'components/mp2731/include'}",
                        str(ROOT / "tests/aico_policy_host.c"),
                        str(ROOT / "components/mp2731/mp2731_aico.c"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result.stdout

    def test_at_most_one_reset_per_interval_over_a_day(self):
        self.run_case("interval")

    def test_aico_off_only_looks_hourly_and_counts_nothing(self):
        self.run_case("off")

    def test_failure_retries_after_five_minutes_and_is_not_a_reset(self):
        self.run_case("failed")

    def test_veto_holds_a_due_reset_and_is_counted_once_per_wait(self):
        self.run_case("veto")

    def test_operator_switch_and_manual_reset_restart_the_period(self):
        self.run_case("operator")

    def test_render_is_valid_json_omits_unscanned_fields_and_worst_case_fits(self):
        out = records(self.run_case("render"))
        fresh = json.loads(out["RENDER_FRESH"])
        self.assertEqual(fresh, {"enabled": True, "iin_dpm_ma": 800, "aico_stat": True,
                                 "resets": 0, "failed": 0, "last_reset_ms": None})
        self.assertEqual(json.loads(out["RENDER_UNSCANNED"]),
                         {"resets": 0, "failed": 0, "last_reset_ms": None})
        self.assertEqual(json.loads(out["RENDER_PARTIAL"]),
                         {"iin_dpm_ma": 800, "resets": 0, "failed": 0, "last_reset_ms": None})
        worst = json.loads(out["RENDER_WORST"])
        self.assertEqual(worst["iin_dpm_ma"], 3250)
        self.assertLess(len(out["RENDER_WORST"]), 192)


class DriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="aico-driver-")
        directory = Path(cls.tmp.name)
        (directory / "device_commands.h").write_text(DEVICE_COMMANDS_STUB)
        cls.binary = directory / "driver"
        cjson = cjson_dir()
        subprocess.run([compiler(), *FLAGS, "-DESP_ERR_TIMEOUT=0x107",
                        f"-I{directory}", f"-I{ROOT / 'tests/mp2731_driver_stubs'}",
                        f"-I{ROOT / 'tests/host_stubs'}", f"-I{cjson}",
                        *(f"-I{ROOT / 'components' / name / 'include'}" for name in
                          ("domain", "mp2731", "CLI", "command_router")),
                        str(ROOT / "tests/aico_host.c"),
                        str(ROOT / "components/mp2731/mp2731_core.c"),
                        str(ROOT / "components/mp2731/mp2731_diagnostics.c"),
                        str(ROOT / "components/mp2731/mp2731_redetect.c"),
                        str(ROOT / "components/mp2731/mp2731_aico.c"),
                        str(ROOT / "components/CLI/cli_aico.c"),
                        str(ROOT / "components/command_router/aico_command.c"),
                        str(cjson / "cJSON.c"), "-lm", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result.stdout

    def test_switch_changes_only_reg02_bit0_with_verified_readback(self):
        self.run_case("set")

    def test_hourly_rearm_rewrites_reg00_unchanged_and_restores_iin_dpm(self):
        charger = json.loads(records(self.run_case("auto"))["CHARGER"])
        aico = charger["aico"]
        self.assertEqual((aico["enabled"], aico["iin_dpm_ma"], aico["aico_stat"], aico["resets"],
                          aico["failed"]), (True, 1000, True, 2, 0))
        self.assertIsInstance(aico["last_reset_ms"], int)
        self.assertIn("redetect", charger)
        self.assertLessEqual(len(json.dumps(charger, separators=(",", ":"))), 2048)

    def test_aico_off_fleet_never_writes_and_reads_reg02_hourly(self):
        self.run_case("off")

    def test_maintenance_manual_redetect_and_dirty_bg_en_veto_the_rearm(self):
        self.run_case("veto")

    def test_rearm_waits_for_an_automatic_redetect_outcome(self):
        self.run_case("interplay")

    def test_bus_failures_are_counted_retried_and_never_reported_as_resets(self):
        self.run_case("failures")

    def test_manual_reset_counts_restarts_the_hour_and_keeps_hiz_en_ilim(self):
        self.run_case("manual")

    def test_verified_mutation_with_failed_status_read_is_recorded_not_replayed(self):
        self.run_case("status_fail")

    def test_cli_parses_get_on_off_reset(self):
        self.run_case("cli")

    def test_mqtt_defers_to_worker_and_reports_registers_and_counters(self):
        out = self.run_case("mqtt")
        reply = json.loads(records(out)["REPLY"])
        self.assertEqual(set(reply), {"type", "id", "ok", "enabled", "reg02", "status_valid", "iin_limit_ma", "iin_dpm_ma",
                                      "aico_stat", "resets", "failed", "last_reset_ms", "next_check_in_s",
                                      "detail"})

    def test_mqtt_rejects_bad_input_and_busy(self):
        self.run_case("mqtt_invalid")


if __name__ == "__main__":
    unittest.main()

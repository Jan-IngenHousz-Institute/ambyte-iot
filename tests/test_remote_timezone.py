"""Execute the production timezone command and scheduler offset engine."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RemoteTimezoneTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="remote-timezone-")
        directory = Path(cls.temp.name)
        cls.binary = directory / "timezone-command"
        # Same installed pinned SDK used by firmware builds. No substitute JSON
        # parser or copy of the router/zone logic is used by this host test.
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home()/".platformio"))/"packages"
        sdk = packages/"framework-espidf@3.50500.0"
        if not sdk.exists():
            sdk = packages/"framework-espidf"
        cjson = sdk/"components/json/cJSON"
        if not (cjson/"cJSON.c").exists():
            raise RuntimeError("Pinned ESP-IDF cJSON is required for remote timezone qualification")
        (directory/"esp_log.h").write_text("#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n")
        subprocess.run([
            os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-Dsetenv=host_setenv",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g", "-pthread",
            f"-I{directory}", f"-I{ROOT/'tests/host_stubs'}", f"-I{cjson}",
            *(f"-I{ROOT/'components'/name/'include'}" for name in ("command_router","timezone","time_sync","device_config")),
            str(ROOT/"tests/timezone_command_host.c"), str(ROOT/"components/command_router/timezone_command.c"),
            str(ROOT/"components/command_router/device_location.c"),
            str(ROOT/"components/timezone/timezone.c"), str(ROOT/"components/time_sync/time_sync.c"), str(cjson/"cJSON.c"), "-lm", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, case):
        run = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        return run.stdout

    def test_persistent_idempotent_runtime_apply_and_read_only_verification(self):
        reply = json.loads(self.run_case("apply"))
        self.assertEqual(reply["timezone"], "Australia/Darwin")
        self.assertEqual(reply["active_timezone"], "Australia/Darwin")
        self.assertEqual(reply["utc_offset_seconds"], 34200)
        self.assertTrue(reply["applied"])

    def test_invalid_values_do_not_dispatch_or_write(self):
        self.run_case("invalid")

    def test_queued_and_foreign_maintenance_busy_never_writes(self):
        self.run_case("busy")

    def test_persistence_failure_keeps_previous_runtime(self):
        self.run_case("persist_fail")

    def test_readback_failure_is_not_claimed_as_applied(self):
        self.run_case("readback_fail")

    def test_offset_readers_are_safe_during_runtime_zone_changes(self):
        self.run_case("threads")

    def test_invalid_stored_zone_is_repairable(self):
        self.run_case("repair")

    def test_unset_zone_can_be_read_and_then_configured(self):
        self.run_case("unset")

    def test_setenv_failure_keeps_previous_zone_and_is_retried(self):
        self.run_case("setenv_fail")

    def test_libc_running_another_rule_restores_previous_zone(self):
        self.run_case("libc_reject")

    def test_position_change_keeps_exact_half_hour_offset(self):
        self.run_case("position")

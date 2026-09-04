"""Host-side contract checks for the sched_spec component and sched_host CLI."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/sched_spec"
INCLUDES = [
    ROOT / "components/sched_spec/include",
    ROOT / "components/time_sync/include",
    ROOT / "tests/host_stubs",
]
COMPONENT_SRCS = sorted((ROOT / "components/sched_spec").glob("*.c")) + [
    ROOT / "components/time_sync/time_sync.c"
]
ACTIONS = [
    "ambit/trace",
    "ambit/spectrum",
    "ambit/leaf-temp",
    "ambit/actinic",
    "device/status-report",
    "db/store-event",
    "device/log",
    "device/sleep",
]
SITE = ["--lat", "52.173", "--lon", "5.819"]


def compile_host(binary: Path, main: Path) -> None:
    compile_cmd = [
        os.environ.get("CC", shutil.which("clang") or "cc"),
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        *(f"-I{inc}" for inc in INCLUDES),
        str(main),
        *(str(src) for src in COMPONENT_SRCS),
        "-lm",
        "-o",
        str(binary),
    ]
    subprocess.run(compile_cmd, check=True, cwd=ROOT)


class SchedSpecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="sched-spec-host-")
        cls.unit = Path(cls.tmp.name) / "sched_spec_host"
        cls.cli = Path(cls.tmp.name) / "sched_host"
        compile_host(cls.unit, ROOT / "tests/sched_spec_host.c")
        compile_host(cls.cli, ROOT / "tools/sched_host.c")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        env = os.environ.copy()
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        return subprocess.run(
            [str(self.cli), *args], cwd=ROOT, capture_output=True, text=True, env=env
        )

    def test_unit_host_binary(self) -> None:
        env = os.environ.copy()
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(
            [str(self.unit)], cwd=ROOT, capture_output=True, text=True, env=env
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("FAIL", result.stdout)
        self.assertIn("SIZES ", result.stdout)
        self.assertRegex(result.stdout, r"SCHED_SPEC_HOST_OK \d+ checks")

    def test_check_valid_fixtures(self) -> None:
        valid = (
            "schedule_default.yaml",
            "schedule_legacy_1hz_spec.yaml",
            "schedule_channels_forms.yaml",
            "schedule_every_90s.yaml",
        )
        for name in valid:
            with self.subTest(fixture=name):
                result = self.run_cli("--check", str(FIXTURES / name))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("OK "), result.stdout)

    def test_bad_fixtures_rejected_with_line_col(self) -> None:
        bad = sorted(FIXTURES.glob("bad_*.yaml"))
        self.assertGreaterEqual(len(bad), 20)
        for fixture in bad:
            with self.subTest(fixture=fixture.name):
                result = self.run_cli("--check", str(fixture))
                self.assertNotEqual(result.returncode, 0, fixture.name)
                self.assertRegex(result.stderr, r":\d+:\d+:", fixture.name)

    def test_schema_dump_is_json_with_all_actions(self) -> None:
        result = self.run_cli("--schema")
        self.assertEqual(result.returncode, 0, result.stderr)
        schema = json.loads(result.stdout)
        self.assertIsInstance(schema, dict)
        for action in ACTIONS:
            self.assertIn(action, result.stdout)
        # channels is optional on the ambit/* actions (absent = all present)
        for branch in schema["oneOf"]:
            uses = branch["properties"]["uses"]["const"]
            required = branch["properties"]["with"]["required"]
            if uses.startswith("ambit/"):
                self.assertNotIn("channels", required, uses)
            if uses == "ambit/trace":
                self.assertEqual(required, ["protocol"])
            # kind is required on store-event (T3 review: NULL deref on device)
            if uses == "db/store-event":
                self.assertIn("kind", required, uses)
        # an action with required inputs must require the outer `with`,
        # otherwise { uses: ambit/trace } would pass the schema while the
        # compiler rejects it (review): trace/actinic/store-event/log/sleep
        with_required = {"ambit/trace", "ambit/actinic", "db/store-event",
                         "device/log", "device/sleep"}
        for branch in schema["oneOf"]:
            uses = branch["properties"]["uses"]["const"]
            outer_required = branch["required"]
            self.assertIn("uses", outer_required, uses)
            if uses in with_required:
                self.assertIn("with", outer_required, uses)
            else:
                self.assertNotIn("with", outer_required, uses)

    def test_simulate_summer(self) -> None:
        result = self.run_cli(
            "--simulate", str(FIXTURES / "schedule_default.yaml"),
            "--date", "2026-06-21", "--tz-offset-s", "7200", *SITE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        out = result.stdout
        self.assertIn("sunrise 05:16:43 local, sunset 22:02:43 local", out)
        self.assertRegex(out, r"steady_state\s+1439 fire\(s\)\s+00:01\.\.23:59 every 60s")
        self.assertRegex(out, r"spectra\s+202 fire\(s\) 05:16:43")
        self.assertRegex(out, r"saturating_flash\s+102 fire\(s\) 05:16:43")
        self.assertRegex(out, r"dark_edge\s+2 fire\(s\) 04:46:43 22:32:43")
        self.assertRegex(out, r"health\s+3 fire\(s\) boot 05:01:43 22:17:43")

    def test_simulate_winter(self) -> None:
        result = self.run_cli(
            "--simulate", str(FIXTURES / "schedule_default.yaml"),
            "--date", "2026-01-15", "--tz-offset-s", "3600", *SITE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        out = result.stdout
        self.assertIn("sunrise 08:40:02 local, sunset 16:54:15 local", out)
        self.assertRegex(out, r"steady_state\s+1439 fire\(s\)\s+00:01\.\.23:59 every 60s")
        self.assertRegex(out, r"spectra\s+99 fire\(s\) 08:40:02")
        self.assertRegex(out, r"saturating_flash\s+50 fire\(s\) 08:40:02")
        self.assertRegex(out, r"dark_edge\s+2 fire\(s\) 08:10:02 17:24:15")
        self.assertRegex(out, r"health\s+3 fire\(s\) boot 08:25:02 17:09:15")


if __name__ == "__main__":
    unittest.main()

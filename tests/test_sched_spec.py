"""Host-side contract checks for the sched_spec component and sched_host CLI."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
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

    def check_source(self, name: str, source: str) -> subprocess.CompletedProcess:
        path = Path(self.tmp.name) / f"{name}.yaml"
        path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
        return self.run_cli("--check", str(path))

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

    def test_table_driven_authoring_examples(self) -> None:
        cases = {
            "seconds_cron": """
                schema: jii.ambyte-schedule/v1-draft
                jobs:
                  sample:
                    schedule:
                      cron: "*/5 * * * * *"
                    steps:
                      - uses: ambit/spectrum
                        with:
                          channels:
                            - 0
                            - 2
            """,
            "solar_and_boot": """
                schema: jii.ambyte-schedule/v1-draft
                jobs:
                  health:
                    schedule:
                      - boot
                      - solar: sunrise
                        offset: -15m
                    steps:
                      - uses: device/status-report
            """,
        }
        for name, source in cases.items():
            with self.subTest(case=name):
                result = self.check_source(name, source)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("OK "), result.stdout)

    def test_every_released_schedule_compiles(self) -> None:
        catalog = sorted((ROOT / "schedule").glob("*.yaml"))
        self.assertTrue(catalog, "schedule catalog is empty")
        for schedule in catalog:
            with self.subTest(schedule=schedule.name):
                result = self.run_cli("--check", str(schedule))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith("OK "), result.stdout)

    def test_released_schedules_use_the_cron_first_block_contract(self) -> None:
        catalog = sorted((ROOT / "schedule").glob("*.yaml"))
        self.assertTrue(catalog, "schedule catalog is empty")
        for schedule in catalog:
            with self.subTest(schedule=schedule.name):
                text = schedule.read_text(encoding="utf-8")
                self.assertNotRegex(text, r"(?m)^\s*[^#\n]*[\[\]{}]")
                self.assertNotRegex(text, r"(?m)^\s+on:")
                self.assertNotRegex(text, r"(?m)^\s+(every|interval|at|weekly|sun):")
                self.assertIn("\n    schedule:\n", text)

    def test_table_driven_invalid_documents_report_a_location(self) -> None:
        job = """
            schema: jii.ambyte-schedule/v1-draft
            jobs:
              j:
                schedule:
                  cron: "* * * * *"
                steps:
                  - uses: device/log
                    with:
                      message: hi
        """
        cases = {
            "flow_map": job.replace('cron: "* * * * *"',
                                    'cron: { expression: "* * * * *" }'),
            "flow_sequence": job.replace("steps:\n", "steps: [device/log]\n"),
            "old_interval": job.replace('cron: "* * * * *"', "interval: 1s"),
            "old_at": job.replace('cron: "* * * * *"', "at: 08:00"),
            "four_field_cron": job.replace('"* * * * *"', '"* * * *"'),
            "bad_second": job.replace('"* * * * *"', '"60 * * * * *"'),
            "unknown_action": job.replace("device/log", "device/nope"),
            "missing_required": job.replace("                    with:\n"
                                            "                      message: hi\n", ""),
            "duplicate_key": job.replace("                      message: hi\n",
                                         "                      message: hi\n"
                                         "                      message: again\n"),
            "block_scalar": job.replace("message: hi", "message: |"),
            "yaml_tag": job.replace("message: hi", "message: !env hi"),
            "yaml_alias": job.replace("message: hi", "message: *shared"),
        }
        for name, source in cases.items():
            with self.subTest(case=name):
                result = self.check_source(f"bad_{name}", source)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertRegex(result.stderr, r":\d+:\d+:", name)

    def test_compiler_caps_are_generated_from_the_contract(self) -> None:
        def log_job(name: str, steps: int = 1, message: str = "hi") -> str:
            body = [f"  {name}:", "    schedule:", '      cron: "* * * * *"',
                    "    steps:"]
            for _ in range(steps):
                body += ["      - uses: device/log", "        with:",
                         f"          message: {message}"]
            return "\n".join(body) + "\n"

        header = "schema: jii.ambyte-schedule/v1-draft\n"
        jobs_17 = header + "jobs:\n" + "".join(log_job(f"j{i}") for i in range(17))
        triggers_9 = header + "jobs:\n  j:\n    schedule:\n" + "".join(
            f'      - cron: "{i} * * * *"\n' for i in range(9)
        ) + "    steps:\n      - uses: device/log\n        with:\n          message: hi\n"
        steps_9 = header + "jobs:\n" + log_job("j", steps=9)
        protocols_9 = header + "protocols:\n" + "".join(
            f"  P{i}:\n    - pulses: 1\n      freq: 1\n      actinic: 0\n" for i in range(9)
        ) + "jobs:\n" + log_job("j")
        segments_17 = header + "protocols:\n  P:\n" + "".join(
            "    - pulses: 1\n      freq: 1\n      actinic: 0\n" for _ in range(17)
        ) + "jobs:\n" + log_job("j")
        event_keys_17 = header + "jobs:\n  j:\n    schedule:\n" \
            '      cron: "* * * * *"\n    steps:\n' \
            "      - uses: db/store-event\n        with:\n          kind: e\n          data:\n" + \
            "".join(f"            d{i}: 1\n" for i in range(17))
        dense_step = "      - uses: db/store-event\n        with:\n          channel: 0\n" \
            "          kind: e\n          data:\n" + \
            "".join(f"            d{i}: 1\n" for i in range(16)) + \
            "          metadata:\n" + \
            "".join(f"            m{i}: 1\n" for i in range(16))
        entries_257 = header + "jobs:\n  j:\n    schedule:\n" \
            '      cron: "* * * * *"\n    steps:\n' + dense_step * 8
        long_message = "x" * 180
        string_pool = header + "jobs:\n" + "".join(
            log_job(f"j{i}", steps=2, message=long_message) for i in range(16)
        )
        cases = {
            "jobs": (jobs_17, "jobs; the cap"),
            "triggers": (triggers_9, "schedule entries; the cap"),
            "steps": (steps_9, "steps; the cap"),
            "protocols": (protocols_9, "protocol cap"),
            "segments": (segments_17, "has 17 segments"),
            "event_keys": (event_keys_17, "at most 16 keys"),
            "entries": (entries_257, "entry pool exhausted"),
            "string_pool": (string_pool, "program string pool exhausted"),
        }
        for name, (source, message) in cases.items():
            with self.subTest(cap=name):
                result = self.check_source(f"cap_{name}", source)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertIn(message, result.stderr)
                self.assertRegex(result.stderr, r":\d+:\d+:")

    def test_schema_dump_is_json_with_all_actions(self) -> None:
        result = self.run_cli("--schema")
        self.assertEqual(result.returncode, 0, result.stderr)
        schema = json.loads(result.stdout)
        checked_in = json.loads(
            (ROOT / "schedule/actions.schema.json").read_text(encoding="utf-8")
        )
        self.assertEqual(checked_in, schema,
                         "regenerate schedule/actions.schema.json with sched_host --schema")
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
            "--simulate", str(ROOT / "schedule/default.yaml"),
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
            "--simulate", str(ROOT / "schedule/default.yaml"),
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

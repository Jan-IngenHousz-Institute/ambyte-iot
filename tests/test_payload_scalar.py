"""Host-side contract checks for the payload scalar scanner.

Compiles the REAL production payload_scalar.c plus the real payload_v3.c
builders (tests/payload_scalar_host.c) so the scanner is exercised against
actual builder bytes — not a Python reconstruction of the payload shapes.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PayloadScalarTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="payload-scalar-host-")
        binary = Path(cls.tmp.name) / "payload_scalar_host"
        compile_cmd = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{ROOT / 'components/payload_codec/include'}",
            str(ROOT / "tests/payload_scalar_host.c"),
            str(ROOT / "components/payload_codec/payload_scalar.c"),
            str(ROOT / "components/payload_codec/payload_v3.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(compile_cmd, check=True, cwd=ROOT)
        env = os.environ.copy()
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(
            [str(binary)], check=True, cwd=ROOT, capture_output=True, text=True,
            env=env,
        )
        cls.stdout = result.stdout
        cls.fixtures = dict(
            line.split("=", 1)
            for line in result.stdout.splitlines()
            if line.startswith(("TRACE=", "SPECTRUM=", "TELEMETRY="))
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_host_unit_checks_pass(self) -> None:
        self.assertNotIn("FAIL", self.stdout)
        self.assertRegex(self.stdout, r"PAYLOAD_SCALAR_HOST_OK \d+ checks")

    def test_fixtures_are_real_builder_output(self) -> None:
        # The fixtures the host scanned are the canonical v3 families — if a
        # builder's key layout changes, the scanner's structural (not
        # positional) contract still holds and the host checks above decide.
        self.assertEqual(json.loads(self.fixtures["TRACE"])["schema"],
                         "ambit.trace/3")
        self.assertEqual(json.loads(self.fixtures["SPECTRUM"])["schema"],
                         "ambit.spectrum/1")
        self.assertEqual(json.loads(self.fixtures["TELEMETRY"])["schema"],
                         "ambyte.telemetry/1")
        # the telemetry depth-exactness fixture must keep its channel/sensor_id
        # nested-only, or the host's CHECK_ABSENT checks stop proving anything
        tele = json.loads(self.fixtures["TELEMETRY"])
        self.assertNotIn("channel", tele)
        self.assertNotIn("sensor_id", tele)
        # nested two levels down (health.attached_sensors[]): the depth-exact
        # match has to survive both the object and the array hop
        self.assertEqual(tele["health"]["attached_sensors"][0]["channel"], "uart_0")
        self.assertEqual(tele["health"]["attached_sensors"][0]["sensor_id"],
                         "10:91:A8:4F:4F:D4")


if __name__ == "__main__":
    unittest.main()

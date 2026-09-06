"""Host-side contract checks for the publish envelope's workbook provenance.

Compiles the REAL production envelope_provenance.c plus the real
DC_*_ENVELOPE_FMT macros (tests/envelope_provenance_host.c) so the format
strings the device publishes with — not a Python reconstruction — are what
renders the envelopes asserted below.
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
WB_ID = "1c7b82b5-a0d4-4b1d-bcd6-6e0b7d470040"

# The exact pre-provenance v3 envelope (same gateway state as
# tests/test_payload_v3.py::_published_envelope_raw). A schedule whose header
# declares no workbook provenance must publish byte-identically to today.
V3_NOPROV_REFERENCE = (
    '{"sample":[{"schema":"ambit.trace/3","measure_id":26337}],'
    '"timestamp":"2026-08-05T19:26:00Z",'
    '"device_battery":3.912,"timezone":"Europe/Amsterdam",'
    '"device_id":"28:37:2F:FF:E7:04","device_name":"AmbyteOnAir",'
    '"device_version":"V003","device_firmware":"1.6.6"}'
)


class EnvelopeProvenanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="envelope-prov-host-")
        binary = Path(cls.tmp.name) / "envelope_provenance_host"
        compile_cmd = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{ROOT / 'components/device_commands/include'}",
            f"-I{ROOT / 'components/domain/include'}",
            f"-I{ROOT / 'tests/host_stubs'}",
            str(ROOT / "tests/envelope_provenance_host.c"),
            str(ROOT / "components/device_commands/envelope_provenance.c"),
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
        cls.envelopes = dict(
            line.split("=", 1)
            for line in result.stdout.splitlines()
            if line.startswith(("V3_", "V2_", "GZ_"))
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_host_unit_checks_pass(self) -> None:
        self.assertNotIn("FAIL", self.stdout)
        self.assertRegex(self.stdout, r"ENVELOPE_PROVENANCE_HOST_OK \d+ checks")

    def test_rendered_envelopes_are_valid_json_with_provenance(self) -> None:
        for key in ("V3_FULL", "V2_FULL", "GZ_FULL"):
            with self.subTest(envelope=key):
                env = json.loads(self.envelopes[key])
                self.assertEqual(env["workbook_version_id"], WB_ID)
                self.assertEqual(len(env["macros"]), 2)
                self.assertEqual(
                    env["macros"][0],
                    {
                        "id": "47b03f78-a0d4-4b1d-bcd6-6e0b7d470040",
                        "name": "ambyte-trace-0",
                        "filename": "macro_00000000",
                    },
                )
                # provenance sits after timezone, before device_id, in every
                # format — raw key order is the wire contract
                raw = self.envelopes[key]
                self.assertLess(raw.index('"workbook_version_id"'),
                                raw.index('"device_id"'))

    def test_no_provenance_envelope_is_byte_identical_to_today(self) -> None:
        raw = self.envelopes["V3_NOPROV"]
        self.assertEqual(raw, V3_NOPROV_REFERENCE)
        self.assertNotIn("workbook_version_id", raw)
        self.assertNotIn('"macros"', raw)


if __name__ == "__main__":
    unittest.main()

"""Real telemetry builder → ingest envelope, including per-row workbook routing."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TelemetryPublishTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "telemetry_publish"
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio"))) / "packages"
        cjson = sorted(packages.glob("framework-espidf*/components/json/cJSON/cJSON.c"))[0]
        subprocess.run([
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            *[f"-I{ROOT / directory}" for directory in (
                "tests/heartbeat_stubs", "tests/host_stubs", "components/domain/include",
                "components/device_commands/include", "components/payload_codec/include")],
            f"-I{cjson.parent}", str(cjson),
            *[str(ROOT / source) for source in (
                "tests/telemetry_publish_host.c", "components/device_commands/telemetry_publish.c",
                "components/device_commands/envelope_provenance.c",
                "components/payload_codec/payload_scalar.c", "components/payload_codec/payload_v3.c")],
            "-lm", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_scenario(self, name):
        return subprocess.run([str(self.binary), name], check=True, text=True, capture_output=True).stdout

    def test_ingest_contract_without_external_power_or_storage(self):
        for scenario in ("battery", "critical", "charging", "power_error", "clock_unset", "no_schedule", "optional"):
            with self.subTest(scenario=scenario):
                envelope = json.loads(self.run_scenario(scenario))
                sample = envelope["sample"][0]
                self.assertEqual(sample["schema"], "ambyte.telemetry/1")
                self.assertEqual(sample["measure_id"], 9007199254740993)
                self.assertNotIn("type", envelope)
                self.assertFalse(sample["health"]["connectivity"]["publish_gate"])
                self.assertFalse(sample["health"]["storage"]["db_online"])
                if scenario != "power_error":
                    self.assertEqual(sample["health"]["power"]["input_present"], scenario == "charging")
                if scenario not in ("no_schedule", "optional"):
                    self.assertEqual(envelope["workbook_version_id"], "test-workbook-id")
                    self.assertEqual([m["id"] for m in envelope["macros"]], ["telemetry-id"])
                else:
                    self.assertNotIn("workbook_version_id", envelope)
                    self.assertNotIn("macros", envelope)
                if scenario != "optional":
                    self.assertEqual(envelope["device_name"], 'Ambyte "test"\n')
                    self.assertEqual(envelope["timezone"], "Europe/Amsterdam")
                if scenario == "power_error":
                    self.assertNotIn("device_battery", envelope)
                    self.assertEqual(sample["health"]["power"], {})
                else:
                    self.assertAlmostEqual(envelope["device_battery"], 3.1 if scenario == "critical" else 3.8)
                self.assertEqual(envelope["timestamp"], "1970-01-01T00:00:00Z" if scenario == "clock_unset" else "2026-09-18T07:00:00Z")

    def test_every_cjson_allocation_failure_and_transport_failure(self):
        self.run_scenario("allocations")
        self.run_scenario("publish_failure")

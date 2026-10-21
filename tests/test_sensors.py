# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the production inventory and serializer, with only hardware ports fake."""
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("empty", "filled", "stale", "fresh_boundary", "absent", "swap",
             "busy", "veto", "reset", "reconnect", "spec_bare", "spec_silent",
             "stable", "old", "bad_values", "slice", "clocks", "zero_crc",
             "exhaustion", "epoch", "bad_format", "cal_flap", "seeded", "version_gate", "documentation")


def build(binary):
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "components/sensor_inventory/include"),
                    str(ROOT / "components/sensor_inventory/sensor_json.c"),
                    str(ROOT / "tests/sensors_host/driver.c"), "-pthread", "-lm", "-o", str(binary)], check=True)


class SensorsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="sensors-host-")
        cls.binary = Path(cls.temp.name) / "sensors"
        build(cls.binary)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        result = subprocess.run([self.binary, name], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_all_production_state_scenarios(self):
        for name in SCENARIOS:
            with self.subTest(name=name):
                for snapshot in self.run_case(name):
                    self.assertEqual(snapshot["contract"], 1)
                    self.assertEqual([p["port"] for p in snapshot["ports"]], list(range(4)))

    def test_observation_clocks_and_rollback(self):
        snapshots = self.run_case("clocks")
        for source, snapshot in zip(("unavailable", "rtc", "hwm", "sntp"), snapshots):
            stamp = snapshot["ports"][0]["sensor_id"]
            self.assertEqual(stamp["clock"], None if source == "unavailable" else {"source": source, "suspect": source == "hwm"})
            self.assertEqual(stamp["observed_utc_ms"] is None, source == "unavailable")
        earlier, rollback = (s["ports"][0]["sensor_id"] for s in snapshots[-2:])
        self.assertLess(rollback["observed_utc_ms"], earlier["observed_utc_ms"])
        self.assertGreater(rollback["observed_uptime_ms"], earlier["observed_uptime_ms"])
        zero = self.run_case("zero_crc")[0]["ports"][0]
        self.assertEqual(zero["calibration_digest"]["value"]["hex"], "00000000")
        self.assertIsNotNone(zero["calibration"]["value"])

    def test_documented_populated_example_is_production_serializer_output(self):
        result = subprocess.run([self.binary,"documentation"],capture_output=True,check=True)
        docs = (ROOT / "docs/usb-maintenance.md").read_text()
        example = docs.split("<!-- sensors-populated-example:start -->",1)[1].split(
            "<!-- sensors-populated-example:end -->",1)[0].split("```json\n",1)[1].split("\n```",1)[0]
        self.assertEqual(example.encode(), result.stdout.strip())
        ports = json.loads(example)["ports"]
        self.assertEqual([p["state"] for p in ports],["known","absent","busy","unknown"])
        self.assertIsNotNone(ports[0]["calibration"]["value"])
        self.assertIsNotNone(ports[0]["spectral"]["value"])
        self.assertEqual(ports[3]["stale_reason"],"never_observed")

    def test_exact_calibration_bytes_crc_and_endianness(self):
        snapshot = self.run_case("filled")[0]
        for ch, port in enumerate(snapshot["ports"]):
            self.assertEqual(port["hw_rev"]["value"], 7)
            cal = port["calibration"]["value"]
            raw = base64.b64decode(cal["raw_base64"], validate=True)
            self.assertEqual(len(raw), 140)
            self.assertEqual(port["calibration_digest"]["value"]["hex"], f"{zlib.crc32(raw):08x}")
            self.assertEqual(cal["mlx_coef"], [-1000*ch-i for i in range(14)])
            self.assertEqual(cal["actinic_coef"], ch + .25)
            self.assertEqual(port["spectral"]["value"]["par_intercept"], ch + 32)
            self.assertEqual(port["calibration"]["source"], "last_read")
            self.assertEqual(port["sensor_id"]["clock"], {"source": "sntp", "suspect": False})

    def test_unknown_absence_busy_and_stale_are_distinct(self):
        self.assertEqual(self.run_case("empty")[0]["ports"][0]["stale_reason"], "never_observed")
        self.assertFalse(self.run_case("fresh_boundary")[0]["ports"][0]["stale"])
        self.assertEqual(self.run_case("stale")[0]["ports"][0]["stale_reason"], "age")
        self.assertEqual(self.run_case("busy")[0]["ports"][0]["state"], "busy")
        absent = self.run_case("absent")[0]["ports"][0]
        self.assertEqual(absent["state"], "absent")
        self.assertIsNone(absent["sensor_id"]["value"])

    def test_bad_float_and_utf8_are_unknown_not_repaired(self):
        port = self.run_case("bad_values")[0]["ports"][0]
        self.assertIsNone(port["calibration"]["value"]["ambit_name"])
        self.assertIsNone(port["calibration"]["value"]["temp_offset"])
        self.assertIsNone(port["spectral"]["value"]["spec_offset"][0])

    def test_reconnect_drops_entire_old_block_and_increments_generation(self):
        invalid, reconnect = self.run_case("reconnect")
        self.assertIsNone(invalid["ports"][0]["calibration"]["value"])
        self.assertEqual(reconnect["ports"][0]["generation"], 2)
        self.assertIsNone(reconnect["ports"][0]["calibration"]["value"])


if __name__ == "__main__":
    unittest.main()

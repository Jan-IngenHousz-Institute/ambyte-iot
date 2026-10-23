"""Whole controller, trace, announcement tracker and NVS adapter against A3 bytes."""
import hashlib
import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Exact A3 candidate sources; provenance is recorded beside these fixtures.
# The candidate was squash-merged, so its original commit is absent in CI.
A3_REFERENCE = ROOT / "tests/fixtures/sensors/a3_6a57c2a"
A3_SHA256 = {
    "device_commands.c": "0b6d99b90a7d24f672d3e37190358d3c0d66d4e57036486e7408c33dd003f8b5",
    "ambit_trace.c": "8f9c6628c9468d1edc92e37d75a888d0601a4a77a474536e86d2ce8f96ccac5b",
}


class SensorControllerTest(unittest.TestCase):
    def test_real_controller_zero_writes_and_a3_trace_golden(self):
        with tempfile.TemporaryDirectory(prefix="sensor-controller-") as name:
            tmp = Path(name)
            # Only SDK declarations differ from firmware. Neither production
            # functions nor their algorithms are extracted or rewritten.
            overlay = tmp / "overlay.h"
            overlay.write_text('#include "freertos/semphr.h"\n'
                               '#define taskENTER_CRITICAL(m) h_enter_critical(m)\n'
                               '#define taskEXIT_CRITICAL(m) h_exit_critical(m)\n'
                               'void vSemaphoreDelete(SemaphoreHandle_t);\n')
            includes = [ROOT / "tests/evq_integ/stubs", ROOT / "tests/evq_host"]
            includes += list((ROOT / "components").glob("*/include"))
            includes += [ROOT / "components/sd_card", ROOT / "components/sync_runner"]
            common = [os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                      "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-O1", "-g",
                      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                      "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                      "-Wl,--wrap=gettimeofday", "-include", str(overlay),
                      *(f"-I{p}" for p in includes)]
            sources = [ROOT / "components/sensor_inventory/sensor_inventory.c",
                       ROOT / "components/sensor_inventory/sensor_json.c",
                       ROOT / "components/ambit_announcement/ambit_announcement.c",
                       ROOT / "components/ambit_announcement_nvs/ambit_announcement_nvs.c",
                       ROOT / "components/payload_codec/payload_v3.c",
                       ROOT / "components/payload_codec/payload_scalar.c"]
            outputs = []
            for reference in (False, True):
                trace = ROOT / "components/ambit_trace/ambit_trace.c"
                flags = []
                if reference:
                    flags = ["-DA3_REFERENCE"]
                    for part, path in (("commands", "device_commands.c"),
                                       ("trace", "ambit_trace.c")):
                        target = tmp / f"a3-{part}.c"
                        source = (A3_REFERENCE / path).read_bytes()
                        self.assertEqual(hashlib.sha256(source).hexdigest(), A3_SHA256[path],
                                         f"A3 reference fixture bytes changed: {path}")
                        target.write_bytes(source)
                    flags.append(f'-DCOMMANDS_SOURCE="{tmp / "a3-commands.c"}"')
                    trace = tmp / "a3-trace.c"
                binary = tmp / ("baseline" if reference else "candidate")
                build = subprocess.run([*common, *flags, str(ROOT / "tests/sensors_host/controller_driver.c"),
                                        str(trace), *(str(s) for s in sources), "-lm", "-o", str(binary)],
                                       capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                if not reference:
                    for fw_size, cal_size in ((48,139),(48,0),(47,140)):
                        with self.subTest(fw_size=fw_size, cal_size=cal_size):
                            fault = subprocess.run([binary,"legacy-short",str(fw_size),str(cal_size)],
                                                   capture_output=True, timeout=30)
                            self.assertEqual(fault.returncode,0,fault.stderr.decode())
                            port = json.loads(fault.stdout)["ports"][0]
                            for field in ("calibration", "calibration_digest", "spectral"):
                                self.assertIsNone(port[field]["value"])
                                self.assertIsNone(port[field]["source"])
                            self.assertEqual(port["sensor_id"]["value"] is None,fw_size!=48)
                    for scenario in ("recovery-jobs", "recovery-stalled"):
                        with self.subTest(scenario=scenario):
                            recovery = subprocess.run([binary, scenario], capture_output=True, timeout=30)
                            self.assertEqual(recovery.returncode, 0, recovery.stderr.decode())
                            model = json.loads(recovery.stdout)
                            self.assertEqual(model["ports"][0]["state"], "stalled" if scenario.endswith("stalled") else "known")
                            self.assertEqual(model["refresh"]["wire_transactions"][0], 0)
                            self.assertGreater(model["acquisition_recovery"]["wire_transactions"][0], 0)
                    partial = subprocess.run([binary,"partial-trigger"],capture_output=True,timeout=30)
                    self.assertEqual(partial.returncode,0,partial.stderr.decode())
                    snapshot = json.loads(partial.stdout)
                    self.assertEqual([p["state"] for p in snapshot["ports"][:2]],["busy","known"])
                    self.assertEqual(snapshot["refresh"]["wire_transactions"],[0]*4)
                run = subprocess.run([binary], capture_output=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
                if not reference:
                    self.assertIn(b"events_delta=0 setters_delta=0 commits_delta=0", run.stderr)
                outputs.append(run.stdout)
            self.assertEqual(len(outputs[0].splitlines()), 10)
            self.assertEqual(outputs[0], outputs[1])

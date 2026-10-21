"""Whole controller, trace, announcement tracker and NVS adapter against A3 bytes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = "6a57c2ae155597a81dc1b0a010824c7e9336f0a7"


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
                    for part, path in (("commands", "components/device_commands/device_commands.c"),
                                       ("trace", "components/ambit_trace/ambit_trace.c")):
                        target = tmp / f"a3-{part}.c"
                        target.write_bytes(subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT))
                    flags.append(f'-DCOMMANDS_SOURCE="{tmp / "a3-commands.c"}"')
                    trace = tmp / "a3-trace.c"
                binary = tmp / ("baseline" if reference else "candidate")
                build = subprocess.run([*common, *flags, str(ROOT / "tests/sensors_host/controller_driver.c"),
                                        str(trace), *(str(s) for s in sources), "-lm", "-o", str(binary)],
                                       capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                run = subprocess.run([binary], capture_output=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
                if not reference:
                    self.assertIn(b"events_delta=0 setters_delta=0 commits_delta=0", run.stderr)
                outputs.append(run.stdout)
            self.assertEqual(len(outputs[0].splitlines()), 10)
            self.assertEqual(outputs[0], outputs[1])

"""The complete production runner holds inspection ownership over every job."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SensorRunnerTest(unittest.TestCase):
    def test_whole_runner_job_action_sleep_failure_and_stop(self):
        with tempfile.TemporaryDirectory(prefix="sensor-runner-") as name:
            tmp = Path(name)
            (tmp / "default_yaml_embed.h").write_text('static const char default_yaml_start[]="";\nstatic const unsigned default_yaml_size=0;\n')
            includes = [ROOT / "tests/identity_host/include", ROOT / "tests/host_stubs", tmp,
                        ROOT / "components/sched_runner"]
            includes += list((ROOT / "components").glob("*/include"))
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-g", "-O1",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *(f"-I{p}" for p in includes),
                str(ROOT / "tests/sensors_host/runner_driver.c"),
                str(ROOT / "components/sensor_inventory/sensor_inventory.c"),
                *(str(p) for p in (ROOT / "components/sched_spec").glob("*.c")),
                str(ROOT / "components/time_sync/time_sync.c"), "-lm", "-o", str(tmp / "runner")],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([tmp / "runner"], capture_output=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr.decode())
            self.assertIn(b"complete_job_veto=1", run.stdout)

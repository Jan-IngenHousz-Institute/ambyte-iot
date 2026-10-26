"""Execute production worker/admission functions, with FreeRTOS queue ports fake."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_schedule_light_cleanup import function

ROOT = Path(__file__).resolve().parents[1]


class SensorWorkerTest(unittest.TestCase):
    def test_real_worker_phase_latencies_and_every_maintenance_op_veto(self):
        app = (ROOT / "main/app_main.c").read_text()
        ota = (ROOT / "components/ambit_ota/ambit_ota.c").read_text()
        start = app.index("typedef struct {\n    void (*run)(void *arg);")
        end = app.index("} maint_job_t;", start) + len("} maint_job_t;")
        exact = [app[start:end]] + [function(app, name) for name in (
            "app_maintenance_begin", "app_maintenance_end", "app_maint_submit",
            "app_sensors_wake", "app_maint_worker")]
        runner = (ROOT / "components/sched_runner/sched_runner.c").read_text()
        exact.append(function(runner, "sched_runner_exit_cleanup"))
        exact += ["#define s_cfg ambit_cfg", function(ota, "ambit_ota_run"), "#undef s_cfg"]
        with tempfile.TemporaryDirectory(prefix="sensor-worker-") as name:
            tmp = Path(name)
            (tmp / "worker-production.inc").write_text("\n".join(exact))
            binary = tmp / "worker"
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra",
                "-Werror", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                f"-I{tmp}", f"-I{ROOT / 'components/sensor_inventory/include'}",
                str(ROOT / "components/sensor_inventory/sensor_inventory.c"),
                str(ROOT / "tests/sensors_host/worker_driver.c"), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([binary], capture_output=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr.decode())
            self.assertIn(b"job_wait_us=900000", run.stdout)

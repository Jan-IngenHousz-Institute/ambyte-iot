"""Real Jobs/OTA admission and retry timer with the production service lease."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from pinned_idf import pinned_idf

ROOT = Path(__file__).resolve().parents[1]


class ServiceJobsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        IDF = pinned_idf()
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "jobs"
        includes = [ROOT / "tests/svc_host/jobs_include", ROOT / "tests/svc_host/include",
                    ROOT / "tests/identity_host/include", ROOT / "tests/wifi_manager_stubs",
                    IDF / "components/json/cJSON"] + list((ROOT / "components").glob("*/include"))
        p = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DSVC_HOST",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            *(f"-I{path}" for path in includes),
            *(str(ROOT / "components/usb_service" / name) for name in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c", "svc_session.c")),
            str(ROOT / "components/iot_jobs/iot_jobs.c"), str(ROOT / "components/iot_jobs/iot_jobs_plan.c"),
            str(ROOT / "components/ota_update/ota_update.c"), str(IDF / "components/json/cJSON/cJSON.c"),
            str(ROOT / "components/sensor_inventory/sensor_inventory.c"),
            str(ROOT / "components/sensor_inventory/sensor_json.c"),
            str(ROOT / "tests/svc_host/jobs_driver.c"), "-Wl,--wrap=svc_store_open", "-lcrypto", "-o", str(cls.binary)],
            capture_output=True, text=True)
        if p.returncode: raise RuntimeError(p.stdout + p.stderr)

    @classmethod
    def tearDownClass(cls): cls.build.cleanup()

    def test_valid_jobs_stay_queued_without_report_and_retry_after_lease_release(self):
        for case in ("success", "auth", "timeout", "queue_failure"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                p = subprocess.run([str(self.binary), case], capture_output=True,
                    env={**os.environ, "SVC_TEST_ROOT": str(Path(temporary) / "svc")})
                self.assertEqual(p.returncode, 0, p.stderr.decode())
                self.assertIn(b"JOBS_OK queue_untouched=1 retry_us=600000000 reports_during_valid_trial=0", p.stderr)


if __name__ == "__main__": unittest.main()

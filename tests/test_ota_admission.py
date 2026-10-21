import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
IDF=Path.home()/".platformio/packages/framework-espidf@3.50500.0"
if not IDF.exists():
    IDF=Path.home()/".platformio/packages/framework-espidf"
if not (IDF/"version.txt").exists() or (IDF/"version.txt").read_text().strip()!="5.5.0":
    raise RuntimeError("Install the pinned espressif32@6.12.0 / IDF5.5.0 packages before host tests")


class AdmissionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.binary=Path(cls.temp.name)/"admission"
        includes=[ROOT/"tests/ota_host/include",ROOT/"tests/svc_host/jobs_include",ROOT/"tests/svc_host/include",
                  ROOT/"tests/identity_host/include",ROOT/"tests/wifi_manager_stubs",IDF/"components/json/cJSON"]
        includes+=list((ROOT/"components").glob("*/include"))
        sources=[ROOT/"tests/ota_host/update_driver.c",ROOT/"components/ota_policy/ota_manifest.c",
                 ROOT/"components/ota_policy/ota_policy.c",ROOT/"components/iot_jobs/iot_jobs.c",
                 ROOT/"components/iot_jobs/iot_jobs_plan.c",IDF/"components/json/cJSON/cJSON.c"]
        p=subprocess.run([os.environ.get("CC","clang"),"-std=c11","-D_POSIX_C_SOURCE=200809L",
            "-Wall","-Wextra","-Werror","-Wno-unused-function","-fsanitize=address,undefined","-g","-O1",
            *(f"-I{i}" for i in includes),*map(str,sources),"-o",str(cls.binary)],capture_output=True,text=True)
        if p.returncode:raise RuntimeError(p.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def run_case(self,path,scenario,cycles=1):
        p=subprocess.run([str(self.binary),path,scenario,str(cycles)],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr)
        return p.stdout.strip().removeprefix("TRACE=")

    def test_real_jobs_and_https_admission_order_and_no_latch_on_refusal(self):
        for path in ("https","jobs"):
            prefix="PCVHSRM" if path=="jobs" else "PCSRM"
            with self.subTest(path=path):self.assertEqual(self.run_case(path,"accept"),prefix+"FLX")
            for reason in ("signature","incompatible","persist_fail","incomplete","legacy_error","running_bad","candidate_bad"):
                with self.subTest(path=path,reason=reason):
                    trace=self.run_case(path,reason)
                    self.assertTrue(trace.endswith("AWNDE"+("B" if path=="jobs" else "")),trace)
            for reason in ("bad_version","bad_hash"):
                if path=="jobs":
                    with self.subTest(path=path,reason=reason):self.run_case(path,reason)

    def test_twenty_refusal_cycles_release_allocations_and_admission(self):
        for path in ("https","jobs"):
            with self.subTest(path=path):self.assertEqual(self.run_case(path,"incompatible",20).count("D"),20)

    def test_signed_app_cannot_api_rollback_to_unsigned_forces_restart(self):
        self.assertEqual(self.run_case("health","rollback_error"),"KX")

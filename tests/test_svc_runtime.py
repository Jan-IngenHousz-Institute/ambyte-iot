# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual ESP service adapter/CLI entry and main admission predicate on host."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


class ServiceRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "runtime"
        includes = [ROOT / "tests/svc_host/include", ROOT / "tests/identity_host/include",
                    ROOT / "tests/wifi_manager_stubs"] + list((ROOT / "components").glob("*/include"))
        p = subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-DSVC_HOST",
            *(f"-I{path}" for path in includes),
            *(str(ROOT / "components/usb_service" / name) for name in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c", "svc_session.c")),
            str(ROOT / "tests/svc_host/runtime_driver.c"), "-Wl,--wrap=svc_store_open", "-lcrypto", "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if p.returncode: raise RuntimeError(p.stdout + p.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def test_runtime_outcomes_metrics_and_legacy_filter_release(self):
        for case, state, reason in (("success", "succeeded", ""), ("auth", "failed", "wifi_auth_rejected"),
                                    ("timeout", "failed", "wifi_timeout"), ("queue_failure", "failed", "busy")):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                p = subprocess.run([str(self.binary), case], capture_output=True,
                                   env={**os.environ, "SVC_TEST_ROOT": str(Path(temporary) / "svc")})
                self.assertEqual(p.returncode, 0, p.stderr.decode())
                responses = {}
                for line in p.stdout.splitlines(keepends=True):
                    prefix, crc = line[:-1].rsplit(b"*", 1)
                    self.assertEqual(crc, b"%08x" % zlib.crc32(prefix))
                    responses[int(prefix[:8], 16)] = json.loads(prefix[9:])
                self.assertEqual((responses[5]["state"], responses[5]["reason"]), (state, reason))
                self.assertEqual((responses[6]["free_heap"], responses[6]["worker_stack"], responses[6]["cli_stack"]), (64000, 4444, 8888))
                self.assertEqual(responses[7]["configured_ssid"], "original")
                for path in (Path(temporary) / "svc").iterdir():
                    self.assertNotIn(b"wrongpass", path.read_bytes())
                    self.assertNotIn(b"d3JvbmdwYXNz", path.read_bytes())
                self.assertNotIn(b"wrongpass", p.stdout)
                self.assertNotIn(b"d3JvbmdwYXNz", p.stdout)

    def test_all_three_legacy_configurations_use_filter_and_service_bypasses(self):
        source = (ROOT / "main/app_main.c").read_text()
        for type_name in ("ota_update_config_t", "ambit_ota_config_t", "script_update_config_t"):
            body = re.search(type_name + r"\s+\w+\s*=\s*\{(.*?)\n    \};", source, re.S).group(1)
            self.assertRegex(body, r"\.submit\s*=\s*app_maint_submit_legacy\b")
        service = re.search(r"usb_service_config_t\s+\w+\s*=\s*\{(.*?)\n    \};", source, re.S).group(1)
        self.assertRegex(service, r"\.submit\s*=\s*app_maint_submit\b")


if __name__ == "__main__":
    unittest.main()

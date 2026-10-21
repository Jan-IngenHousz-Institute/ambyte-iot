"""Real manager/engine Wi-Fi persistence with inline/queued IDF disconnects."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ServiceWifiEngineTest(unittest.TestCase):
    def test_persist_rejoin_succeeds_only_after_new_owned_ip(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "wifi-service"
            includes = [ROOT / "tests/wifi_manager_stubs", ROOT / "components/wifi_manager",
                        ROOT / "components/wifi_manager/include", ROOT / "components/usb_service/include"]
            p = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                *(f"-I{x}" for x in includes), str(ROOT / "tests/svc_host/wifi_engine_driver.c"),
                                *(str(ROOT / "components/usb_service" / name) for name in
                                  ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c")), "-lcrypto", "-o", str(binary)],
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            for case in ("inline", "queued", "stale"):
                with self.subTest(case=case):
                    p = subprocess.run([str(binary), str(Path(temporary) / case), case], capture_output=True, text=True)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    result = json.loads(p.stdout)
                    if case != "stale":
                        self.assertEqual((result["state"], result["saved"], result["connected"], result["retrying"]),
                                         ("succeeded", True, True, False))
                    else:
                        self.assertEqual((result["state"], result["saved"], result["connected"]), ("uncertain", None, False))
                    for secret in ("trial-pass", "dHJpYWwtcGFzcw=="):
                        self.assertNotIn(secret, p.stdout + p.stderr)
                        for file in (Path(temporary) / case).iterdir(): self.assertNotIn(secret.encode(), file.read_bytes())

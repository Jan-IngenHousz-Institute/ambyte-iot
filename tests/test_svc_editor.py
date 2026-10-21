"""Qualify notice recovery against the actual pinned linenoise, not a model."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

from pinned_idf import pinned_idf

ROOT = Path(__file__).resolve().parents[1]


class ServiceEditorTest(unittest.TestCase):
    def test_real_editor_cancels_stray_line_reprints_prompt_and_accepts_identity(self):
        sdk = pinned_idf()
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-DCONFIG_IDF_TARGET_LINUX=1",
                       "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                       "-Wno-sign-compare",  # pinned vendor's int/sizeof loop on 64-bit host
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                       "-I" + str(sdk / "components/console"),
                       "-I" + str(ROOT / "tests/svc_host/sdk_include"),
                       "-I" + str(ROOT / "tests/svc_host/include"),
                       "-I" + str(ROOT / "components/usb_service/include"),
                       str(ROOT / "components/usb_service/svc_wire.c"),
                       str(ROOT / "tests/svc_host/editor_driver.c"),
                       str(sdk / "components/console/linenoise/linenoise.c"), "-o", str(out / "editor")]
            compiled = subprocess.run(command, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr.decode())
            for mode in ("dumb", "raw"):
                with self.subTest(mode=mode):
                    history = out / (mode + "-history")
                    p = subprocess.run([out / "editor", history, mode], capture_output=True, timeout=10)
                    self.assertEqual(p.returncode, 0, p.stderr.decode())
                    self.assertIn(b"\r\n00000000 ", p.stdout)
                    line = next(line for line in p.stdout.splitlines(keepends=True) if line.startswith(b"00000000 "))
                    body, crc = line[:-1].rsplit(b"*", 1)
                    self.assertEqual(int(crc, 16), zlib.crc32(body))
                    self.assertIn(b"REAL_EDITOR_IDENTITY_OK", p.stdout)
                    self.assertGreaterEqual(p.stdout.count(b"ambyte> "), 2)
                    self.assertEqual(history.read_bytes(), b"identity\n")
                    self.assertNotIn(b"wrongpass", p.stdout)
                    self.assertNotIn(b"d3JvbmdwYXNz", p.stdout)
                    self.assertNotIn(b"unframed producer", p.stdout)

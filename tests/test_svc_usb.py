# SPDX-License-Identifier: GPL-3.0-or-later
"""Production USB interposition, with explicit driver-boundary models."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ServiceUsbTest(unittest.TestCase):
    def test_scoped_driver_hooks_and_editor(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "usb"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I" + str(ROOT / "tests/svc_host/include"),
                "-I" + str(ROOT / "components/usb_service/include"),
                str(ROOT / "components/usb_service/svc_wire.c"),
                str(ROOT / "tests/svc_host/usb_driver.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

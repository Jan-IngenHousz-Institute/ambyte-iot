# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""tools/bench_flash.py preserves NVS by default and writes it only with --provision."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load():
    spec = importlib.util.spec_from_file_location("bench_flash", ROOT / "tools/bench_flash.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class BenchFlashTest(unittest.TestCase):
    def run_tool(self, *args, environ=None):
        mod = load()
        with mock.patch.dict(mod.os.environ, environ or {}, clear=False), \
             mock.patch.object(mod.subprocess, "run") as run, \
             mock.patch.object(mod.Path, "unlink"):
            self.assertEqual(mod.main(["--port", "/dev/ttyACM9", *args]), 0)
        return run.call_args_list

    def test_default_preserves_nvs_for_build_and_upload(self):
        calls = self.run_tool()
        pio = [c for c in calls if c.args[0][0] == "pio"]
        self.assertEqual(len(pio), 2)
        for c in pio:
            self.assertEqual(c.kwargs["env"].get("AMBYTE_NVS_SKIP"), "1")

    def test_provision_writes_nvs_even_if_the_shell_set_skip(self):
        calls = self.run_tool("--provision", environ={"AMBYTE_NVS_SKIP": "1"})
        pio = [c for c in calls if c.args[0][0] == "pio"]
        self.assertEqual(len(pio), 2)
        for c in pio:
            self.assertNotIn("AMBYTE_NVS_SKIP", c.kwargs["env"])

    def test_always_bench_signs_before_upload(self):
        calls = self.run_tool()
        cmds = [" ".join(map(str, c.args[0])) for c in calls]
        sign = next(i for i, c in enumerate(cmds) if "sign_firmware.py" in c)
        upload = next(i for i, c in enumerate(cmds) if "upload" in c)
        self.assertLess(sign, upload)
        self.assertIn("--allow-bench", cmds[sign])


if __name__ == "__main__":
    unittest.main()

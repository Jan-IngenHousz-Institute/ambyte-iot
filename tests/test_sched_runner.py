"""Host-side checks for the pure parts of the sched_runner component.

Compiles tests/sched_runner_host.c against sched_spec + the extracted
db/store-event JSON writer (components/sched_runner/sched_runner_json.c) and
runs it. Pattern: tests/test_sched_spec.py.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
INCLUDES = [
    ROOT / "components/sched_runner",  # sched_runner_priv.h
    ROOT / "components/sched_spec/include",
    ROOT / "components/time_sync/include",
    ROOT / "tests/host_stubs",
]
COMPONENT_SRCS = sorted((ROOT / "components/sched_spec").glob("*.c")) + [
    ROOT / "components/time_sync/time_sync.c",
    ROOT / "components/sched_runner/sched_runner_json.c",
]


class SchedRunnerHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="sched-runner-host-")
        cls.unit = Path(cls.tmp.name) / "sched_runner_host"
        compile_cmd = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            *(f"-I{inc}" for inc in INCLUDES),
            str(ROOT / "tests/sched_runner_host.c"),
            *(str(src) for src in COMPONENT_SRCS),
            "-lm",
            "-o",
            str(cls.unit),
        ]
        subprocess.run(compile_cmd, check=True, cwd=ROOT)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_unit_host_binary(self) -> None:
        result = subprocess.run(
            [str(self.unit)], cwd=ROOT, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("FAIL", result.stdout)
        self.assertRegex(result.stdout, r"SCHED_RUNNER_HOST_OK \d+ checks")


if __name__ == "__main__":
    unittest.main()

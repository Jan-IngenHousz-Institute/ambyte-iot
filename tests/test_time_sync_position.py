# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""time_sync position and generation stay consistent across tasks (TSan)."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TimeSyncPositionTest(unittest.TestCase):
    def test_reader_never_sees_new_generation_with_old_position(self):
        with tempfile.TemporaryDirectory(prefix="ts-pos-") as tmp:
            binary = Path(tmp) / "ts-pos"
            subprocess.run([
                os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
                "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=thread", "-pthread",
                f"-I{ROOT / 'tests/host_stubs'}", f"-I{ROOT / 'components/time_sync/include'}",
                str(ROOT / "tests/time_sync_position_host.c"),
                str(ROOT / "components/time_sync/time_sync.c"), "-lm", "-o", str(binary),
            ], check=True)
            env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1:exitcode=66")
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120, env=env)
            self.assertEqual(run.returncode, 0, run.stderr[-3000:])
            self.assertIn("TIME_SYNC_POSITION_OK", run.stdout)


if __name__ == "__main__":
    unittest.main()

"""Fast parser allocation peaks cannot disappear between periodic samples."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ServiceCompilerMetricsTest(unittest.TestCase):
    def test_all_actual_parser_allocations_observed_before_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "metrics"
            includes = [ROOT / "components/sched_spec/include", ROOT / "components/time_sync/include", ROOT / "tests/host_stubs"]
            sources = sorted((ROOT / "components/sched_spec").glob("*.c")) + [ROOT / "components/time_sync/time_sync.c"]
            p = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *(f"-I{x}" for x in includes),
                                str(ROOT / "tests/svc_host/compiler_metrics.c"), *map(str, sources),
                                *(f"-Wl,--wrap={name}" for name in ("malloc", "calloc", "realloc", "free")), "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            p = subprocess.run([str(binary), str(ROOT / "components/sched_runner/default.yaml")], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("COMPILER_METRICS_OK", p.stdout)

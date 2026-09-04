# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/bench_diag/bench_diag.c").read_text()
CMAKE = (ROOT / "components/bench_diag/CMakeLists.txt").read_text()
RUNBOOK = (ROOT / "docs/bench/RUNBOOK.md").read_text()


class BenchFloodTest(unittest.TestCase):
    def test_command_is_bench_only(self):
        self.assertIn("if(CONFIG_AMBYTE_BENCH_DIAG)", CMAKE)
        self.assertIn('list(APPEND bench_diag_srcs "bench_diag.c")', CMAKE)
        self.assertIn('.command = "bench_flood"', SOURCE)

    def test_events_use_the_shared_store_path(self):
        flood = SOURCE.split("static void bench_flood_task", 1)[1].split(
            "static int bench_flood_command", 1
        )[0]
        self.assertIn("cmd_next_measure_id(&measure_id)", flood)
        self.assertIn("cmd_store_event(&event)", flood)
        self.assertIn("MEASUREMENT_TAG_MEASUREMENT", flood)
        self.assertIn('.cmd_raw = "bench_flood"', flood)
        self.assertIn("target = (uint64_t)cfg.hz * (uint64_t)cfg.seconds", flood)
        self.assertIn("while (attempted < target)", flood)

    def test_runbook_documents_finite_repro_and_fallback(self):
        self.assertIn("bench_flood 10 1200 16000", RUNBOOK)
        self.assertIn("bench_flood 2 1200 80000", RUNBOOK)


if __name__ == "__main__":
    unittest.main()

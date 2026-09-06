# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/bench_diag/bench_diag.c").read_text()
CMAKE = (ROOT / "components/bench_diag/CMakeLists.txt").read_text()
RUNBOOK = (ROOT / "docs/bench/RUNBOOK.md").read_text()
APP_MAIN = (ROOT / "main/app_main.c").read_text()
CLI = (ROOT / "components/CLI/CLI.c").read_text()
DEVICE_COMMANDS = (ROOT / "components/device_commands/device_commands.c").read_text()
PYPROJECT = (ROOT / "pyproject.toml").read_text()


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

    def test_payload_floor_covers_full_int64_id_and_errors_are_distinct(self):
        self.assertIn("#define BENCH_FLOOD_BYTES_MIN  48U", SOURCE)
        self.assertIn("BENCH flood id fetch failed", SOURCE)
        self.assertIn("BENCH flood payload too small", SOURCE)
        self.assertNotIn("BENCH flood id/payload failed", SOURCE)

    def test_pacing_uses_nonzero_tick_domain_intervals(self):
        self.assertIn("configTICK_RATE_HZ >= BENCH_FLOOD_HZ_MAX", SOURCE)
        self.assertIn("vTaskDelayUntil(&wake, period_ticks)", SOURCE)
        self.assertNotIn("esp_timer_get_time", SOURCE)

    def test_runbook_documents_finite_repro_and_fallback(self):
        self.assertIn("bench_flood 10 1200 16000", RUNBOOK)
        self.assertIn("bench_flood 2 1200 80000", RUNBOOK)
        stop_at = RUNBOOK.index("schedule stop")
        flood_at = RUNBOOK.index("bench_flood 10 1200 16000")
        complete_at = RUNBOOK.index("BENCH flood complete:")
        start_at = RUNBOOK.index("schedule start")
        self.assertLess(stop_at, flood_at)
        self.assertLess(flood_at, complete_at)
        self.assertLess(complete_at, start_at)
        self.assertIn("Use `stored=`", RUNBOOK)
        self.assertNotIn("last measure_id - first measure_id + 1", RUNBOOK)
        self.assertIn("abort, reboot the bench unit", RUNBOOK)

    def test_bench_registration_failure_does_not_abort_boot(self):
        self.assertNotIn("ESP_ERROR_CHECK(bench_diag_start())", APP_MAIN)
        self.assertIn("bench diagnostics unavailable", APP_MAIN)

    def test_review_contract_comments_and_license_scope(self):
        self.assertIn("repl_config.task_stack_size = 12288", CLI)
        self.assertIn("next notification/fallback wake", DEVICE_COMMANDS)
        self.assertIn("cannot accelerate quarantine", DEVICE_COMMANDS)
        self.assertIn("components/, main/, and schedule/", PYPROJECT)


if __name__ == "__main__":
    unittest.main()

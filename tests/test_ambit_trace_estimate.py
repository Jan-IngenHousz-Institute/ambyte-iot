"""Host test for the AMBIT trace run-time estimate, both acquisition engines.

AMBIT fw < 1.4.0 free-runs the ADPD (a nominal 1 Hz pulse takes tick_factor s,
below 1); fw >= 1.4.0 paces every pulse from the ESP clock at exactly 1/freq
plus ~0.5 s of setup, so the shipped 45-pulse SS run grows from ~41.2 s to
~44.4 s. The scheduler must never poll inside the run (the AMBIT cannot answer
and the stray wake bytes desync the next poll/fetch), so the estimate has to be
an upper bound per engine and the runner has to pick it per channel. Nergena
2026-10-05: 2.5.2 gateways polled 1.4.0 AMBITs at 90 % of the free-run estimate
and channel 0 lost its SS traces for hours.

Two parts:
  - tests/ambit_trace_estimate_host.c runs the extracted production helpers
    (ambit_trace_engine_for / ambit_trace_estimate_ms_for / ambit_trace_estimate_ms)
    against the SS and MPF protocols of schedule/default.yaml as compiled by the
    production schedule compiler, plus every protocol of the catalog;
  - source-shape checks pin the runner invariant: one per-channel estimate from
    the single helper drives both the poll start (>= 100 %) and the deadline.

Pattern: tests/test_schedule_light_cleanup.py (function extraction, so the
ESP-IDF half of ambit_trace.c stays out of the host binary).
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?(?:const )?[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class TraceEstimateTest(unittest.TestCase):
    def test_estimate_both_engines(self):
        trace = (ROOT / "components/ambit_trace/ambit_trace.c").read_text(encoding="utf-8")
        extracted = [function(trace, name) for name in (
            "ambit_trace_engine_for", "ambit_trace_estimate_ms_for", "ambit_trace_estimate_ms")]
        catalog = [ROOT / "schedule/default.yaml"] + sorted(
            p for p in (ROOT / "schedule").glob("*.yaml") if p.name != "default.yaml")
        with tempfile.TemporaryDirectory(prefix="trace-estimate-") as tmp:
            tmp = Path(tmp)
            (tmp / "production.inc").write_text("\n".join(extracted) + "\n", encoding="utf-8")
            binary = tmp / "test"
            includes = [ROOT / "tests/host_stubs", tmp]
            includes += list((ROOT / "components").glob("*/include"))
            sources = list((ROOT / "components/sched_spec").glob("*.c")) + [
                ROOT / "components/time_sync/time_sync.c"]
            subprocess.run([
                os.environ.get("CC", shutil.which("clang") or "cc"),
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                *(f"-I{p}" for p in includes), str(ROOT / "tests/ambit_trace_estimate_host.c"),
                *(str(p) for p in sources), "-lm", "-o", str(binary),
            ], check=True)
            result = subprocess.run([str(binary), *(str(p) for p in catalog)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("FAIL", result.stdout)
            self.assertRegex(result.stdout, r"AMBIT_TRACE_ESTIMATE_HOST_OK \d+ checks")

    def test_runner_polls_per_channel_from_the_single_helper(self):
        actions = (ROOT / "components/sched_runner/sched_runner_actions.c").read_text(encoding="utf-8")
        header = (ROOT / "components/ambit_trace/include/ambit_trace.h").read_text(encoding="utf-8")
        # The 90 % start is gone for good: the estimate is the earliest poll.
        self.assertNotIn("POLL_START_PCT", actions)
        trace_run = function(actions, "act_ambit_trace")
        self.assertNotIn("ambit_trace_estimate_ms(segs", trace_run)
        self.assertIn("int64_t est_ms[UART_SENSOR_NUM_CHANNELS]", trace_run)
        # Identity is read from the cache only, after the trigger acked — the
        # AMBIT is measuring and could not answer a fetch.
        self.assertIn("cmd_ambit_device_info_cached(ch, &info)", trace_run)
        self.assertLess(trace_run.index("ambit_trace_trigger(ch"),
                        trace_run.index("cmd_ambit_device_info_cached(ch, &info)"))
        self.assertIn("ambit_trace_estimate_ms_for(", trace_run)
        self.assertIn("ambit_trace_engine_for(known ? &info : NULL)", trace_run)
        # One per-channel value gates the poll AND bounds the deadline.
        self.assertIn("if (elapsed < est_ms[ch]) continue;", trace_run)
        self.assertIn("elapsed > est_ms[ch] + margin_ms", trace_run)
        self.assertEqual(trace_run.count("ambit_trace_estimate_ms_for("), 1)
        # Unknown identity → the longer (paced) estimate, documented at the helper.
        self.assertIn("An UNKNOWN identity is treated as PACED", header)
        self.assertIn("AMBIT_TRACE_PACED_FW_MINOR       4", header)


if __name__ == "__main__":
    unittest.main()

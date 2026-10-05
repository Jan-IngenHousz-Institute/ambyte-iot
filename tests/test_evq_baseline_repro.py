"""A1: the released baseline (b3f9b8a, v2.4.2) LOSES measurements when its
small internal store fills while the SD card has room.

Compiles `git show b3f9b8a:components/event_log/event_log.c` (path literals
rewritten only; see evidence rewrite) against the same media shim as the
fix, runs the outage scenario and PASSES ONLY IF the loss reproduces:
NO_MEM refusals, accepted < generated, zero unsent records on SD.
"""
from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "evq_host"))

import scenarios as S  # noqa: E402

SEEDS = [int(x) for x in os.environ.get("EVQ_SEEDS", "1,7,1337").split(",") if x]


class BaselineReproducesLoss(unittest.TestCase):
    def test_A1_baseline_refuses_while_sd_has_room(self):
        for seed in SEEDS:
            with self.subTest(seed=seed):
                r = S.A1(seed)
                self.assertGreater(r["refused_no_mem"], 0)
                self.assertLess(r["accepted"], r["generated"])
                self.assertEqual(r["unsent_records_on_sd"], 0)


if __name__ == "__main__":
    unittest.main()

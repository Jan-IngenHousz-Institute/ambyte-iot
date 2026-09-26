"""Sprint-01 storage contract (groups A2-H, Q) on the PRODUCTION event store.

Compiles components/event_log/{event_log,evq_index,evq_render}.c with clang
ASan+UBSan against the tests/evq_host media shim and runs every contract
scenario for seeds 1, 7, 1337 (override with EVQ_SEEDS). All scenario x seed
runs execute in parallel in setUpModule; each test method asserts its rows.
Evidence: $EVQ_OUT/<scenario>/<seed>/ ; scratch: $EVQ_TMPDIR (kept on failure).
The fault matrix (group D) is run separately: tests/evq_host/run.py --matrix.
"""
from __future__ import annotations

import concurrent.futures as cf
import multiprocessing as mp
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "evq_host"))

import run as R  # noqa: E402
import scenarios as S  # noqa: E402

SEEDS = [int(x) for x in os.environ.get("EVQ_SEEDS", "1,7,1337").split(",") if x]
NAMES = [n for g, ns in R.GROUPS.items() for n in ns if n != "A1"]
RESULTS: dict = {}


def setUpModule() -> None:
    R._prebuild()
    tasks = [(n, s) for n in NAMES for s in SEEDS]
    jobs = max(1, (os.cpu_count() or 2) - 2)
    with cf.ProcessPoolExecutor(max_workers=jobs, mp_context=mp.get_context("fork")) as ex:
        for r in ex.map(R._one, *zip(*tasks)):
            RESULTS[(r["name"], r["seed"])] = r


class EvqStorageContract(unittest.TestCase):
    pass


def _make(name: str):
    def test(self):
        bad = [RESULTS[(name, s)] for s in SEEDS if not RESULTS[(name, s)]["ok"]]
        self.assertFalse(bad, "\n".join(f"seed {b['seed']}: {b['error']}\n{b.get('trace', '')}" for b in bad))
    test.__doc__ = (getattr(S, name).__doc__ or name).strip().splitlines()[0]
    return test


for _n in NAMES:
    setattr(EvqStorageContract, f"test_{_n}", _make(_n))


if __name__ == "__main__":
    unittest.main()

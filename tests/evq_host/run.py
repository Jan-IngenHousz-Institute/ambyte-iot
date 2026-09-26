#!/usr/bin/env python3
"""Storage-harness entry point (tests/evq_host).

  run.py --list-faults                 registered fault points (from the production sources)
  run.py --only A2,B1 --seeds 1,7      run named contract scenarios
  run.py --matrix --seeds 1,7,1337 [--coverage-report]
                                       D matrix: every point x {crash,eio,enospc} x nth {1,2,last}

Evidence goes to $EVQ_OUT (per scenario/seed); scratch to $EVQ_TMPDIR.
Exit status is non-zero if any run fails or (with --coverage-report) any
registered fault point was never reached.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import evq_lib  # noqa: E402
import scenarios as S  # noqa: E402

GROUPS = {
    "A": ["A1", "A2", "A3"], "B": ["B1", "B2", "B3", "B4", "B5"],
    "C": ["C1", "C2", "C3", "C4", "C5", "C6", "C7", "C9"],
    "E": ["E1", "E2", "E3", "E4", "E5", "E6", "E7", "E8", "E9", "E10", "E11", "E12", "E13"],
    "F": ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"],
    "G": ["G1", "G2", "G3", "G4", "G5", "G6", "G7", "G8"],
    "H": ["H1", "H2", "H3", "H4", "H5", "H6"], "Q": ["Q1", "Q2", "Q3"],
}
MODES = ["crash", "eio", "enospc"]


def _one(name: str, seed: int) -> dict:
    t = time.time()
    try:
        r = getattr(S, name)(seed)
        return {"name": name, "seed": seed, "ok": True, "secs": round(time.time() - t, 1),
                "dups": r.get("duplicate_count") if isinstance(r, dict) else None}
    except Exception as e:  # noqa: BLE001
        return {"name": name, "seed": seed, "ok": False, "secs": round(time.time() - t, 1),
                "error": f"{type(e).__name__}: {e}", "trace": traceback.format_exc(limit=4)}


def _matrix_one(args) -> dict:
    point, mode, nth, seed = args
    t = time.time()
    try:
        r = S.matrix_run(point, mode, nth, seed)
        r["ok"] = True
    except Exception as e:  # noqa: BLE001
        r = {"point": point, "mode": mode, "nth": nth, "seed": seed, "ok": False,
             "error": f"{type(e).__name__}: {e}"[:2000], "trace": traceback.format_exc(limit=4)}
    r["secs"] = round(time.time() - t, 1)
    return r


def _prebuild() -> None:
    """Build every driver variant once, before workers fork."""
    S.exe("head")
    S.exe("head", S.MATRIX_TUNING)
    S.exe("head", {"EVQ_INDEX_COMPACT_BYTES": 1024})                            # B4
    S.exe("head", {"EVQ_INDEX_CAP": 6})                                        # C7
    S.exe("head", {"EVLOG_ROTATE_BYTES": 262144, "EVQ_SD_RESERVE_BYTES": 64 * S.MiB})  # A3
    S.exe("b3f9b8a")
    S.exe("v2.2.3")


def run_matrix(seeds: list[int], jobs: int, coverage: bool) -> int:
    points = evq_lib.registered_fault_points()
    _prebuild()
    # dry run per seed sizes nth=last
    last: dict[tuple[int, str], int] = {}
    for seed in seeds:
        dry = S.matrix_dry(seed)
        for p, c in dry["counts"].items():
            last[(seed, p)] = c
    tasks = []
    for seed in seeds:
        for p in points:
            n_last = max(last.get((seed, p), 1), 1)
            for nth in sorted({1, 2, n_last}):
                for mode in MODES:
                    tasks.append((p, mode, nth, seed))
    print(f"matrix: {len(points)} points x {len(MODES)} modes x nth{{1,2,last}} x {len(seeds)} seed(s) = {len(tasks)} runs",
          flush=True)
    results = []
    with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
        for i, r in enumerate(ex.map(_matrix_one, tasks, chunksize=1)):
            results.append(r)
            if not r["ok"]:
                print(f"FAIL {r['point']} {r['mode']} nth={r['nth']} seed={r['seed']}: {r['error'][:400]}", flush=True)
            if (i + 1) % 100 == 0:
                print(f"  {i + 1}/{len(tasks)} done", flush=True)
    reached = set()
    fired = set()
    for r in results:
        reached.update(r.get("reached", []))
        for f in r.get("fired", []):
            fired.add(f.split(":")[0])
    unreached = sorted(set(points) - reached)
    never_fired = sorted({p for p in points} - fired)
    fails = [r for r in results if not r["ok"]]
    summary = {"points": points, "runs": len(results), "failed": len(fails), "unreached": unreached,
               "never_fired": never_fired, "seeds": seeds,
               "max_dups_non_reimport": max([r.get("dups", 0) for r in results if r["ok"] and not r["point"].startswith("reimport.")] + [0]),
               "results": [{k: v for k, v in r.items() if k not in ("reached", "counts", "trace")} for r in results]}
    out = os.environ.get("EVQ_OUT")
    if out:
        d = Path(out) / "D_matrix"
        d.mkdir(parents=True, exist_ok=True)
        (d / "summary.json").write_text(json.dumps(summary, indent=1, sort_keys=True))
        (d / "failures.json").write_text(json.dumps(fails, indent=1))
    print(f"matrix: {len(results) - len(fails)}/{len(results)} passed; unreached={unreached}; never_fired={never_fired}")
    rc = 1 if fails else 0
    if coverage and (unreached or never_fired):
        rc = 1
    return rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list-faults", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--groups", default="")
    ap.add_argument("--seeds", default="1,7,1337")
    ap.add_argument("--matrix", action="store_true")
    ap.add_argument("--coverage-report", action="store_true")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    a = ap.parse_args()
    seeds = [int(x) for x in a.seeds.split(",") if x]
    if a.list_faults:
        for p in evq_lib.registered_fault_points():
            print(p)
        return 0
    if a.matrix:
        return run_matrix(seeds, a.jobs, a.coverage_report)
    names = [n for n in a.only.split(",") if n]
    for g in [g for g in a.groups.split(",") if g]:
        names += GROUPS[g]
    if not names:
        names = [n for g in GROUPS.values() for n in g]
    _prebuild()
    tasks = [(n, s) for n in names for s in seeds]
    fails = 0
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for r in ex.map(_one, *zip(*tasks)):
            status = "PASS" if r["ok"] else "FAIL"
            print(f"{status} {r['name']} seed={r['seed']} {r['secs']}s" + ("" if r["ok"] else f"\n  {r['error'][:1500]}"), flush=True)
            fails += 0 if r["ok"] else 1
    print(f"{len(tasks) - fails}/{len(tasks)} passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

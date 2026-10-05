#!/usr/bin/env python3
"""Sprint-1 RED/GREEN for the event_log SD write-integrity group (contract §5.3).

  red_green.py --rev REV --out DIR [--seeds 1,7,1337] [--only X1,X4] [--jobs N]
  red_green.py --rev REV --out DIR --self-check [...]

Builds two storage-harness drivers:
  base  `git show REV:` of components/event_log/** (+ sd_card.h, domain
        includes) with only the documented path-literal rewrite
        (build.build_rev_full; REV must already have the indexed store,
        e.g. e1ca6ee), and
  head  the working tree (build.build_head),
then runs X1..X10 x seeds on both. Evidence per variant/scenario/seed goes to
DIR/base/<scn>/<seed>/ and DIR/head/<scn>/<seed>/; DIR/summary.json lists every
expectation (scenario, seed, variant, expected, observed signature, pass).

Expectations:
  base X1/X2/X3  RED   >=1 xlink_unlink at the failed commit AND the committed
                       SD copy reads zero (C-13..C-15)
  base X4        RED   the copy that failed read-back is unlinked, no evq/bad-* (C-16)
  base X7        RED   one xlk slot left + spool both_eio: >=1 xlink_unlink or dst unlinked
  base X5/X6     any   recorded only (may be GREEN on base)
  base X8..X10   any   recorded only: their retire.pair.* fault points do not exist on base
  head X1..X10   GREEN every oracle (C-18, C-19, C-20, C-29/C-30, C-50..C-52)
                       and the scenario's GREEN signature

Exit 0 only if every base expectation holds and every head run is GREEN.
--self-check runs the same pipeline with HEAD replaced by BASE and exits 0
only if that swapped run FAILS (proves the script can see RED).
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import sys
import traceback
from pathlib import Path

HOST = Path(__file__).resolve().parent
sys.path.insert(0, str(HOST))

XNAMES = ["X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8", "X9", "X10"]
RED_EXPECTED = {"X1", "X2", "X3", "X4", "X7"}


def _slim(rec: dict) -> dict:
    c19 = rec.get("c19") or {}
    return {"signature": rec.get("signature"), "red": rec.get("red"), "oracle_errors": rec.get("oracle_errors", []),
            "green_errors": rec.get("green_errors", []), "duplicate_count": rec.get("duplicate_count"),
            "missing_ids": len(rec.get("missing_ids", [])), "c19_violations": c19.get("n_violations"),
            "c18_cursor_violations": c19.get("n_c18_cursor")}


def _task(args) -> dict:
    name, seed, kind, variant, out = args
    os.environ["EVQ_OUT"] = out
    import scenarios as S
    try:
        rec = getattr(S, name)(seed, kind=kind, strict=False)
        return {"scenario": name, "seed": seed, "variant": variant, "kind": kind, "ran": True, "rec": _slim(rec)}
    except Exception as e:  # noqa: BLE001 — a harness error is a failed expectation, recorded
        return {"scenario": name, "seed": seed, "variant": variant, "kind": kind, "ran": False,
                "error": f"{type(e).__name__}: {e}"[:3000], "trace": traceback.format_exc(limit=5)}


def _judge(r: dict) -> dict:
    name, variant = r["scenario"], r["variant"]
    expected = ("RED" if name in RED_EXPECTED else "any") if variant == "base" else "GREEN"
    row = {"scenario": name, "seed": r["seed"], "variant": variant, "kind": r["kind"], "expected": expected}
    if not r["ran"]:
        row.update(passed=False, observed=None, detail=r["error"])
        return row
    rec = r["rec"]
    row["observed"] = {"signature": rec["signature"], "red": rec["red"], "oracle_errors": rec["oracle_errors"][:10],
                       "green_errors": rec["green_errors"][:10], "duplicate_count": rec["duplicate_count"]}
    if expected == "RED":
        ok = bool(rec["red"] and rec["red"].get("observed"))
        row.update(passed=ok, detail=("RED observed: " if ok else "RED NOT observed: ") + (rec["red"] or {}).get("rule", ""))
    elif expected == "any":
        rule = (rec["red"] or {}).get("rule") if isinstance(rec["red"], dict) else None
        row.update(passed=True, detail="recorded (no base expectation)" + (f": {rule}" if rule else ""))
    else:
        bad = rec["oracle_errors"] + rec["green_errors"]
        row.update(passed=not bad, detail="GREEN" if not bad else "; ".join(bad[:4]))
    return row


def _git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=HOST.parents[1], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def run_pipeline(base_kind: str, head_kind: str, out: Path, names: list[str], seeds: list[int],
                 jobs: int) -> tuple[int, dict]:
    import scenarios as S
    out.mkdir(parents=True, exist_ok=True)
    S.exe(head_kind)            # build both drivers before forking workers
    S.exe(base_kind)
    tasks = []
    for variant, kind in (("base", base_kind), ("head", head_kind)):
        vdir = out / variant
        vdir.mkdir(parents=True, exist_ok=True)
        tasks += [(n, s, kind, variant, str(vdir)) for n in names for s in seeds]
    rows = []
    with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
        for r in ex.map(_task, tasks, chunksize=1):
            row = _judge(r)
            rows.append(row)
            print(f"{'PASS' if row['passed'] else 'FAIL'} {row['variant']:4} {row['scenario']} seed={row['seed']} "
                  f"expected={row['expected']}: {str(row['detail'])[:300]}", flush=True)
    failed = [r for r in rows if not r["passed"]]
    summary = {"base": base_kind, "head": head_kind,
               "base_commit": _git("rev-parse", f"{base_kind[4:]}^{{commit}}") if base_kind.startswith("rev:") else base_kind,
               "head_commit": _git("rev-parse", "HEAD"),
               "head_worktree_dirty": bool(_git("status", "--porcelain", "--untracked-files=no")),
               "scenarios": names, "seeds": seeds, "expectations": len(rows), "failed": len(failed),
               "rows": sorted(rows, key=lambda r: (r["variant"], r["scenario"], r["seed"]))}
    (out / "summary.json").write_text(json.dumps(summary, indent=1, sort_keys=True, default=str))
    print(f"red_green: {len(rows) - len(failed)}/{len(rows)} expectations met -> {out / 'summary.json'}")
    return (1 if failed else 0), summary


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--rev", required=True, help="baseline revision (must contain components/event_log/evq_index.c)")
    ap.add_argument("--out", required=True, help="evidence directory (base/, head/, summary.json)")
    ap.add_argument("--seeds", default="1,7,1337")
    ap.add_argument("--only", default="", help="comma-separated subset of X1..X10")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--self-check", action="store_true",
                    help="run with HEAD replaced by BASE; exit 0 only if that run fails")
    a = ap.parse_args(argv)
    try:
        seeds = [int(x) for x in a.seeds.split(",") if x]
    except ValueError:
        ap.error(f"--seeds must be comma-separated integers, got {a.seeds!r}")
    names = [n for n in a.only.split(",") if n] or list(XNAMES)
    bad = [n for n in names if n not in XNAMES]
    if bad or not seeds:
        ap.error(f"unknown scenario(s) {bad}" if bad else "--seeds is empty")
    if _git("rev-parse", "--verify", "--quiet", f"{a.rev}^{{commit}}") == "":
        ap.error(f"--rev {a.rev!r} is not a commit in this repository")
    base = f"rev:{a.rev}"
    out = Path(a.out).resolve()
    os.environ.setdefault("EVQ_TMPDIR", str(out / "tmp"))
    if a.self_check:
        rc, _ = run_pipeline(base, base, out / "self_check", names, seeds, a.jobs)
        if rc == 0:
            print("SELF-CHECK FAILED: with HEAD replaced by BASE every expectation still passed")
            return 1
        print("self-check OK: with HEAD replaced by BASE the red/green run fails, as it must")
        return 0
    rc, _ = run_pipeline(base, "head", out, names, seeds, a.jobs)
    return rc


if __name__ == "__main__":
    sys.exit(main())

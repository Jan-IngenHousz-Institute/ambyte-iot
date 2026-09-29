#!/usr/bin/env python3
"""C-21: duplicates on S1 never exceed the baseline, per scenario/seed.

  compare_dups.py BASE_EVQ_OUT S1_EVQ_OUT

Both arguments are EVQ_OUT roots written by tests/evq_host/run.py (the base one
by the baseline revision's own runner). For every <scenario>/<seed>/reconcile.json
present in BOTH trees the duplicate count is read — `duplicate_count` (R-E2E
record) or, for multi-run scenarios, the sum of the nested `dups` /
`duplicate_count` values under `runs` / `variants` — and printed as a table.

Exit status is non-zero if
  * any S1 duplicate count exceeds the base count for the same scenario/seed,
  * no scenario/seed is common to both trees,
  * a base scenario/seed is missing from S1 (new X* scenarios exist only in
    S1 and are ignored), or
  * an argument is not a directory.
Scenario/seeds whose records carry no duplicate count on one side are listed
as n/a and not compared.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def dup_count(rec: dict) -> int | None:
    if isinstance(rec.get("duplicate_count"), int):
        return rec["duplicate_count"]
    total = None

    def walk(x) -> None:
        nonlocal total
        if isinstance(x, dict):
            for k, v in x.items():
                if k in ("dups", "duplicate_count") and isinstance(v, int) and not isinstance(v, bool):
                    total = (total or 0) + v
                elif isinstance(v, (dict, list)):
                    walk(v)
        elif isinstance(x, list):
            for y in x:
                walk(y)
    for key in ("runs", "variants"):
        if key in rec:
            walk(rec[key])
    return total


def collect(root: Path) -> dict[tuple[str, str], int | None]:
    out = {}
    for p in sorted(root.glob("*/*/reconcile.json")):
        scn, seed = p.parent.parent.name, p.parent.name
        try:
            out[(scn, seed)] = dup_count(json.loads(p.read_text()))
        except (json.JSONDecodeError, OSError) as e:
            raise SystemExit(f"compare_dups: unreadable {p}: {e}")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("base", help="baseline EVQ_OUT root")
    ap.add_argument("s1", help="S1 EVQ_OUT root")
    a = ap.parse_args(argv)
    base, s1 = Path(a.base), Path(a.s1)
    for p in (base, s1):
        if not p.is_dir():
            ap.error(f"{p} is not a directory")
    b, s = collect(base), collect(s1)
    common = sorted(set(b) & set(s))
    missing = sorted(k for k in b if k not in s and not k[0].startswith("X"))
    worse = []
    print(f"{'scenario':48} {'seed':>6} {'base':>6} {'s1':>6}  status")
    for k in common:
        bv, sv = b[k], s[k]
        if bv is None or sv is None:
            status = "n/a"
        elif sv > bv:
            status = "WORSE"
            worse.append(k)
        else:
            status = "ok"
        print(f"{k[0]:48} {k[1]:>6} {str(bv):>6} {str(sv):>6}  {status}")
    for k in missing:
        print(f"{k[0]:48} {k[1]:>6} {str(b[k]):>6} {'-':>6}  MISSING in S1")
    s1_only = sorted(k for k in s if k not in b)
    compared = sum(1 for k in common if b[k] is not None and s[k] is not None)
    print(f"compare_dups: {len(common)} common scenario/seed(s), {compared} compared, {len(worse)} worse, "
          f"{len(missing)} missing in S1, {len(s1_only)} S1-only")
    if not common:
        print("compare_dups: FAIL - no scenario/seed common to both trees")
        return 1
    if worse or missing:
        print("compare_dups: FAIL")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

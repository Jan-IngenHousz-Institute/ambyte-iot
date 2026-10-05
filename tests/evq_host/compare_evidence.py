#!/usr/bin/env python3
"""J5 determinism: two clean runs of the storage matrix/scenarios must produce
byte-identical reconcile.json and manifests per scenario/seed.

  compare_evidence.py <EVQ_OUT_1> <EVQ_OUT_2>

Compared: every <scenario>/<seed>/{reconcile.json, accepted.jsonl, refused.jsonl,
indeterminate.jsonl, delivered.jsonl, quarantined.jsonl} present in BOTH roots
(the second run may cover a subset, e.g. matrix only). Not compared: ops.jsonl
(diagnostic trace), timing fields, the integration group (threaded; it
asserts its own 3x repeatability).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

FILES = ["reconcile.json", "accepted.jsonl", "refused.jsonl", "indeterminate.jsonl", "delivered.jsonl", "quarantined.jsonl"]
VOLATILE = {"secs"}


def load(p: Path):
    if p.suffix == ".json":
        d = json.loads(p.read_text())
        return _strip(d)
    return p.read_bytes()


def _strip(d):
    if isinstance(d, dict):
        return {k: _strip(v) for k, v in d.items() if k not in VOLATILE}
    if isinstance(d, list):
        return [_strip(x) for x in d]
    return d


def main() -> int:
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    compared = diffs = 0
    for pb in sorted(b.glob("*/*/")):
        rel = pb.relative_to(b)
        if rel.parts[0] in ("integ",):
            continue
        pa = a / rel
        if not pa.exists():
            continue
        for f in FILES:
            fa, fb = pa / f, pb / f
            if not fb.exists() and not fa.exists():
                continue
            compared += 1
            if not fa.exists() or not fb.exists() or load(fa) != load(fb):
                diffs += 1
                print(f"DIFF {rel}/{f}")
    ma, mb = a / "D_matrix" / "summary.json", b / "D_matrix" / "summary.json"
    if ma.exists() and mb.exists():
        ra = {(r["point"], r["mode"], r["nth"], r["seed"]): {k: v for k, v in r.items() if k not in VOLATILE} for r in json.loads(ma.read_text())["results"]}
        rb = {(r["point"], r["mode"], r["nth"], r["seed"]): {k: v for k, v in r.items() if k not in VOLATILE} for r in json.loads(mb.read_text())["results"]}
        compared += 1
        if ra != rb:
            diffs += 1
            bad = [k for k in set(ra) | set(rb) if ra.get(k) != rb.get(k)]
            print(f"DIFF D_matrix/summary.json results ({len(bad)} runs differ, e.g. {sorted(bad)[:3]})")
    print(f"compared {compared} artifact(s): {diffs} difference(s)")
    return 1 if diffs or compared == 0 else 0


if __name__ == "__main__":
    sys.exit(main())

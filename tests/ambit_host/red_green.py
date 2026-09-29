#!/usr/bin/env python3
"""RED/GREEN evidence for the AMBIT OTA staging + ambit_flash preflight fixes (contract §5.4).

  red_green.py --rev e1ca6ee --out DIR [--only AO1,AF2] [--self-check]

Builds the AMBIT writers twice (git show REV: and the working tree),
runs every scenario on both, and writes DIR/{base,head}/<scenario>/ evidence plus
DIR/summary.json. Exit 0 only if every scenario the contract marks base-RED
reproduces its defect on REV and every HEAD run passes all its checks.
--self-check runs HEAD's expectations against the BASE build as well and must
then FAIL (proves the script can detect RED)."""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ambit_lib as L  # noqa: E402
import ambit_scenarios as S  # noqa: E402


def run_one(exe: Path, out: Path, variant: str, name: str) -> dict:
    dev = L.new_dev(exe, out / variant / name / "state")
    try:
        checks, red = S.SCENARIOS[name](dev)
        err = None
    except Exception as e:  # noqa: BLE001 - recorded as a failed run
        checks, red, err = [("scenario raised", False, repr(e))], None, repr(e)
    rec = {"scenario": name, "variant": variant, "base_red": red, "error": err,
           "checks": [{"name": n, "ok": bool(ok), "detail": d} for n, ok, d in checks]}
    (out / variant / name / "result.json").write_text(json.dumps(rec, indent=1, default=str))
    return rec


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rev", required=True, help="baseline git revision (e.g. e1ca6ee)")
    ap.add_argument("--out", required=True, help="evidence directory (created)")
    ap.add_argument("--only", default="", help="comma list of scenarios (default: all)")
    ap.add_argument("--self-check", action="store_true", help="swap HEAD for BASE; must exit non-zero")
    a = ap.parse_args()
    names = [n for n in a.only.split(",") if n] or list(S.SCENARIOS)
    unknown = [n for n in names if n not in S.SCENARIOS]
    if unknown:
        print(f"unknown scenario(s): {unknown}", file=sys.stderr)
        return 2
    out = Path(a.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    base_exe = L.build(out / "build", a.rev)
    head_exe = base_exe if a.self_check else L.build(out / "build", None)
    rows, ok_all = [], True
    for n in names:
        b = run_one(base_exe, out, "base", n)
        h = run_one(head_exe, out, "head", n)
        want_red = n in S.BASE_RED_EXPECTED
        red_ok = (b["base_red"] is True) if want_red else True
        head_ok = h["error"] is None and all(c["ok"] for c in h["checks"]) and len(h["checks"]) > 0
        # A GREEN verdict needs at least one contract check (C-xx); the baseline
        # build only yields placeholder checks, so --self-check must fail.
        head_ok = head_ok and any(c["name"].startswith("C-") for c in h["checks"])
        row = {"scenario": n, "base_red_expected": want_red, "base_red_observed": b["base_red"],
               "red_ok": red_ok, "head_ok": head_ok,
               "head_failed": [c["name"] for c in h["checks"] if not c["ok"]]}
        rows.append(row)
        ok_all = ok_all and red_ok and head_ok
        print(f"{n:5} base_red={b['base_red']!s:5} (expected {'RED' if want_red else '-'}) "
              f"head={'GREEN' if head_ok else 'FAIL ' + ','.join(row['head_failed'])}", flush=True)
    summary = {"rev": a.rev, "self_check": a.self_check, "ok": ok_all, "rows": rows}
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    print("RESULT", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())

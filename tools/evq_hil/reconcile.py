"""Global oracles of the hardware contract (§4.0): HW-INV, ID-UNIQ, IND-OK,
CNT, over console captures (+ optional warehouse rows).

    python reconcile.py --capture CAP [...] [--warehouse WH.json] [--pre PRE_FLASH.json]
                        [--first-test-id N] --out result.json

Obligations:
  ACC  every EVQ_ACC record (exact stored-line sha256 known)
  IND  synthetic records found in an inventory without EVQ_ACC: identified by
       searching the stored-line hash over (run, next k, the fill's pad, id,
       start = previous accept .. +30 s)
  REAL non-synthetic ids >= first-test-id seen in any inventory (first-seen sha)
  PRE  pending lines of the pre-update backup (PRE_FLASH)
Each obligation id < the latest inventory cutoff must have its line sha in the
latest flash inventory or SD record inventory, or be delivered (a warehouse
row that passed D-2, supplied by reconcile_warehouse.py).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hillog  # noqa: E402
import hilpay  # noqa: E402
import manifest  # noqa: E402

SD_EXCLUDE = ("/bad-", "/xlk-")


def _sd_record_lines(inv: dict) -> list[dict]:
    return [ln for ln in inv["lines"]
            if any(ln["path"].startswith(d) for d in ("/sdcard/events", "/sdcard/evq", "/sdcard/archive"))
            and not any(x in ln["path"] for x in SD_EXCLUDE)]


def find_indeterminate(acc: list[dict], fills: dict, pads: dict, seen: dict[int, set]) -> dict[int, dict]:
    """Search each fill's allocation window for durable-but-unconfirmed records."""
    by_run: dict[str, list[dict]] = {}
    for a in acc:
        by_run.setdefault(a["run"], []).append(a)
    acc_ids = {a["id"] for a in acc}
    ind: dict[int, dict] = {}
    for run, f in fills.items():
        pad = pads.get(run)
        if pad is None:
            continue
        rows = sorted(by_run.get(run, []), key=lambda a: a["k"])
        begins = f["begins"]
        k_next = (rows[-1]["k"] + 1) if rows else begins[0]["k_start"]
        t0 = (rows[-1]["start_ms"] // 1000) if rows else None
        base_id = (rows[-1]["id"] + 1) if rows else begins[0]["next_id"]
        if t0 is None:
            continue
        for cand_id in range(base_id, base_id + 8):          # ids allocated around the reset
            if cand_id in acc_ids or cand_id not in seen:
                continue
            pl = hilpay.payload(run, k_next, pad)
            for sec in range(t0, t0 + 31):
                ms = sec * 1000
                h = hilpay.sha(hilpay.stored_line(cand_id, run, ms, ms, pl))
                if h in seen[cand_id]:
                    ind[cand_id] = {"run": run, "k": k_next, "id": cand_id, "start_ms": ms, "line_sha": h}
                    break
    return ind


def evaluate(texts: list[str], warehouse: dict | None = None, pre: dict | None = None,
             first_test_id: int | None = None) -> dict:
    joined = "\n".join(texts)
    m = manifest.parse(texts)
    summ = manifest.verify(m)
    finv = [x for x in hillog.flash_inventories(joined) if x["complete"]]
    sinv = [x for x in hillog.sd_inventories(joined) if x["complete"]]
    res: dict = {"manifest": summ, "errors": [], "counts": {}}
    if not finv:
        res["errors"].append("no complete flash inventory")
        return res
    fl, sl = finv[-1], (sinv[-1] if sinv else {"lines": []})
    all_lines = [ln for inv in finv for ln in inv["lines"]] + [ln for inv in sinv for ln in _sd_record_lines(inv)]
    seen: dict[int, set] = {}
    for ln in all_lines:
        seen.setdefault(ln["id"], set()).add(ln["sha256"])
    for a in m["accepted"]:
        seen.setdefault(a["id"], set()).add(a["line_sha"])
    if pre:
        for ln in pre["storage"]["lines"]:
            if ln.get("id") is not None:
                seen.setdefault(ln["id"], set()).add(ln["sha256"])
    # ID-UNIQ
    multi = {i: sorted(s) for i, s in seen.items() if len(s) > 1}
    if multi:
        res["errors"].append(f"ID-UNIQ: {len(multi)} id(s) with distinct line hashes, e.g. {list(multi.items())[:3]}")
    ref_ids = {r["id"] for r in m["refused"] if r["id"] is not None}
    ref_seen = sorted(ref_ids & set(seen))
    if ref_seen:
        res["errors"].append(f"ID-UNIQ: refused id(s) present in a source: {ref_seen[:10]}")
    # identical duplicate occurrences (legal, counted)
    occ: dict[int, int] = {}
    for ln in [ln for ln in fl["lines"]] + _sd_record_lines(sl):
        occ[ln["id"]] = occ.get(ln["id"], 0) + 1
    res["counts"]["identical_duplicate_ids"] = sum(1 for v in occ.values() if v > 1)
    # IND
    ind = find_indeterminate(m["accepted"], m["fills"], summ["pads"], seen)
    res["indeterminate"] = list(ind.values())
    # REAL
    hil_ids = {a["id"] for a in m["accepted"]} | set(ind)
    real: dict[int, str] = {}
    if first_test_id is not None:
        for inv in finv + sinv:
            for ln in (inv["lines"] if inv in finv else _sd_record_lines(inv)):
                if ln["id"] >= first_test_id and ln["id"] not in hil_ids and ln["id"] not in real:
                    real[ln["id"]] = ln["sha256"]
    res["counts"]["real"] = len(real)
    # HW-INV
    cutoff = fl["cutoff_id"]
    present: dict[int, set] = {}
    for ln in fl["lines"] + _sd_record_lines(sl):
        present.setdefault(ln["id"], set()).add(ln["sha256"])
    delivered = set((warehouse or {}).get("passed_ids", []))
    obligations: dict[int, tuple[str, str]] = {}
    for a in m["accepted"]:
        obligations[a["id"]] = ("ACC", a["line_sha"])
    for i, r in ind.items():
        obligations[i] = ("IND", r["line_sha"])
    for i, h in real.items():
        obligations.setdefault(i, ("REAL", h))
    if pre:
        for ln in pre.get("pending_lines", []):
            obligations.setdefault(ln["id"], ("PRE", ln["sha256"]))
    missing, carried = [], 0
    for i, (kind, h) in sorted(obligations.items()):
        if i >= cutoff:
            carried += 1
            continue
        if h in present.get(i, set()) or i in delivered:
            continue
        missing.append({"id": i, "kind": kind})
    res["counts"].update({"obligations": len(obligations), "checked_below_cutoff": len(obligations) - carried,
                          "carried_forward": carried, "missing": len(missing), "cutoff_id": cutoff,
                          "flash_lines": len(fl["lines"]), "sd_record_lines": len(_sd_record_lines(sl))})
    if missing:
        res["errors"].append(f"HW-INV: {len(missing)} obligation(s) with no copy and no delivery: {missing[:10]}")
    res["ok"] = not res["errors"]
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", nargs="+", required=True)
    ap.add_argument("--warehouse")
    ap.add_argument("--pre")
    ap.add_argument("--first-test-id", type=int)
    ap.add_argument("--out")
    a = ap.parse_args()
    texts = [Path(p).read_text(errors="replace") for p in a.capture]
    wh = json.loads(Path(a.warehouse).read_text()) if a.warehouse else None
    pre = json.loads(Path(a.pre).read_text()) if a.pre else None
    res = evaluate(texts, wh, pre, a.first_test_id)
    if a.out:
        Path(a.out).write_text(json.dumps(res, indent=1, default=str))
    print(json.dumps({"ok": res.get("ok"), "errors": res["errors"], "counts": res["counts"]}, default=str))
    return 0 if res.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())

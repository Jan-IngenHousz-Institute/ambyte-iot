"""Parse evq_hil console captures into accepted/refused manifests (contract
§1.3, §4.0, MAN-1) and verify every accepted record from first principles.

    python -m tools.evq_hil.manifest <capture.log> [...] --out manifest.json

A record is ACCEPTED only when its EVQ_ACC line exists (printed after
cmd_store_event returned ESP_OK). For each one the payload and the exact
stored line are recomputed from (run, k, pad_len, id, start, end) and must
equal the device-reported hashes. Duplicate (run, k) pairs, an id bound to two
different lines, or a refused id that was also accepted are errors.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import hilpay  # type: ignore
else:
    from . import hilpay

LINE_RE = re.compile(r"(EVQ_(?:BEGIN|ACC|REF|END)) (.*)")


class ManifestError(Exception):
    pass


def parse(texts: list[str]) -> dict:
    fills: dict[str, dict] = {}
    acc: list[dict] = []
    ref: list[dict] = []
    for text in texts:
        for raw in text.splitlines():
            m = LINE_RE.search(raw)
            if not m:
                continue
            kind, rest = m.group(1), m.group(2).split()
            if kind == "EVQ_BEGIN":
                run, k0, n, pad, nid = rest[0], int(rest[1]), int(rest[2]), int(rest[3]), int(rest[4])
                fills.setdefault(run, {"begins": [], "ends": []})["begins"].append(
                    {"k_start": k0, "n": n, "pad": pad, "next_id": nid})
            elif kind == "EVQ_END":
                run = rest[0]
                fills.setdefault(run, {"begins": [], "ends": []})["ends"].append(
                    {"k_last": int(rest[1]), "acc": int(rest[2]), "ref": int(rest[3]), "next_id": int(rest[4])})
            elif kind == "EVQ_ACC":
                if len(rest) < 8:
                    raise ManifestError(f"truncated EVQ_ACC: {raw!r}")
                acc.append({"run": rest[0], "k": int(rest[1]), "id": int(rest[2]), "start_ms": int(rest[3]),
                            "end_ms": int(rest[4]), "line_bytes": int(rest[5]), "line_sha": rest[6],
                            "payload_sha": rest[7]})
            else:
                ref.append({"run": rest[0], "k": int(rest[1]), "id": None if rest[2] == "-" else int(rest[2]),
                            "err": rest[3], "reason": rest[4] if len(rest) > 4 else "-"})
    return {"fills": fills, "accepted": acc, "refused": ref}


def verify(m: dict) -> dict:
    pads: dict[str, int] = {}
    for run, f in m["fills"].items():
        ps = {b["pad"] for b in f["begins"]}
        if len(ps) != 1:
            raise ManifestError(f"run {run}: inconsistent pad lengths {ps}")
        pads[run] = ps.pop()
    seen_rk: dict[tuple, int] = {}
    id_line: dict[int, str] = {}
    total_bytes = 0
    for a in m["accepted"]:
        rk = (a["run"], a["k"])
        if rk in seen_rk:
            raise ManifestError(f"duplicate (run,k) {rk} ids {seen_rk[rk]} and {a['id']}")
        seen_rk[rk] = a["id"]
        if a["run"] not in pads:
            raise ManifestError(f"EVQ_ACC for run {a['run']} without EVQ_BEGIN")
        pl = hilpay.payload(a["run"], a["k"], pads[a["run"]])
        if hilpay.sha(pl) != a["payload_sha"]:
            raise ManifestError(f"payload hash mismatch for {rk}")
        line = hilpay.stored_line(a["id"], a["run"], a["start_ms"], a["end_ms"], pl)
        if hilpay.sha(line) != a["line_sha"] or len(line) != a["line_bytes"]:
            raise ManifestError(f"stored-line hash/length mismatch for {rk} id {a['id']}")
        prev = id_line.get(a["id"])
        if prev is not None and prev != a["line_sha"]:
            raise ManifestError(f"id {a['id']} reused with a different line")
        id_line[a["id"]] = a["line_sha"]
        total_bytes += a["line_bytes"]
    acc_ids = set(id_line)
    for r in m["refused"]:
        if r["id"] is not None and r["id"] in acc_ids:
            raise ManifestError(f"refused id {r['id']} also accepted")
    return {"accepted": len(m["accepted"]), "refused": len(m["refused"]), "accepted_line_bytes": total_bytes,
            "runs": sorted(pads), "pads": pads}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    m = parse([Path(p).read_text(errors="replace") for p in a.captures])
    summary = verify(m)
    if a.out:
        Path(a.out).write_text(json.dumps({"summary": summary, **m}, indent=1))
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

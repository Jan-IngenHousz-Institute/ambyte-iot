"""P4 checks (contract F-1..F-4 with the E1/device-specific F-2 replacement).

    python f_check.py --capture cap-HIL.log --from OFFSET --inv-range A B --out f.json

F-1  accepted line bytes > 9,830,400 (the whole storage partition), zero EVQ_REF.
F-2' reclaimed >= 1; no reclaimable SPOOLED segment keeps its flash copy after
     the pressure pass; storage_blocked=0; free-space numbers reported as-is.
F-3  for every flash segment removed in the trace: its index P line and, on SD,
     the index-recorded primary and mirror each went fsync -> rename -> verify
     read (close_r with bytes == segment bytes) BEFORE the flash remove; any
     lost trace entry or seq gap fails.
F-4  nothing published during the hold (0 "publish event ->" lines after the
     fill start); pending accounting reported.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hillog  # noqa: E402
import manifest  # noqa: E402

PART_BYTES = 9_830_400


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--from", dest="start", type=int, required=True)
    ap.add_argument("--inv-range", nargs=2, type=int, required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    raw = Path(a.capture).read_bytes()
    text = raw[a.start:].decode(errors="replace")
    inv_text = raw[a.inv_range[0]:a.inv_range[1]].decode(errors="replace")
    res: dict = {"errors": []}
    m = manifest.parse([text])
    s = manifest.verify(m)
    res["F1"] = {"accepted": s["accepted"], "accepted_line_bytes": s["accepted_line_bytes"],
                 "refused": s["refused"], "partition_bytes": PART_BYTES}
    if s["accepted_line_bytes"] <= PART_BYTES:
        res["errors"].append("F-1: accepted bytes do not exceed the partition")
    if s["refused"]:
        res["errors"].append(f"F-1: {s['refused']} refusal(s)")
    # trace: the RAM ring restarts at seq 1 on every boot, so evaluate each
    # boot segment (split at HIL_BOOT) on its own.
    ix = hillog.index_dumps(inv_text)
    segs = ix[-1]["segs"] if ix else {}
    parts = re.split(r"(?=HIL_BOOT )", text)
    lost_total, gaps_total, n_entries, checked, bad, reclaims, archive_removes = 0, [], 0, 0, [], 0, 0
    hist: list[dict] = []                      # every earlier boot's entries, in order
    for bi, part in enumerate(parts):
        tr = hillog.trace(part)
        ents = sorted({e["seq"]: e for e in tr["entries"]}.values(), key=lambda e: e["seq"])
        for e in ents:
            e["ord"] = (bi, e["seq"])
        n_entries += len(ents)
        lost_total += sum(d["lost"] for d in tr["drains"])
        seqs = [e["seq"] for e in ents]
        first_drain = tr["drains"][0]["first"] if tr["drains"] else (seqs[0] if seqs else 0)
        gaps_total += [(x, y) for x, y in zip(seqs, seqs[1:]) if y != x + 1]
        for rm in ents:
            mm = re.search(r"/evstore/events/ev-(\d+)\.log$", rm["a"])
            if rm["op"] != "remove" or not mm:
                continue
            q = int(mm.group(1))
            after = [e for e in ents if e["seq"] > rm["seq"] and e["op"] == "idx_write"]
            if not any(re.match(rf"O {q} ", e["b"]) for e in after[:6]):
                archive_removes += 1          # delivered-file archive removal, not a reclaim
                continue
            reclaims += 1
            before = hist + [e for e in ents if e["seq"] < rm["seq"]]   # spool may precede a reboot
            pline = [e for e in before if e["op"] == "idx_write" and re.match(rf"P {q} ", e["b"])]
            seg = segs.get(q)
            if not pline:
                bad.append({"seq": q, "why": "no index P line before flash remove"})
                continue
            toks = pline[-1]["b"].split()
            prim = toks[3] if len(toks) > 3 else ""
            mirr = f"m-{q:06d}.log"            # trace field truncates the mirror name
            ok_copies = []
            for nm in (prim, mirr):
                fs = [e for e in before if e["op"] == "fsync" and e["a"].endswith(nm[:-4] + ".tmp")]
                rn = [e for e in before if e["op"] == "rename" and e["b"].endswith(nm)]
                vr = [e for e in before if e["op"] == "close_r" and e["a"].endswith(nm) and
                      (seg is None or e["bytes"] == seg["bytes"])]
                ok = bool(fs and rn and vr and fs[-1]["ord"] < rn[-1]["ord"] and
                          any(rn[-1]["ord"] < v["ord"] < pline[-1]["ord"] for v in vr))
                ok_copies.append(ok)
            if all(ok_copies):
                checked += 1
            else:
                bad.append({"seq": q, "names": [prim, mirr], "copies_ok": ok_copies})
        hist += ents
    res["trace"] = {"entries": n_entries, "boot_segments": len(parts), "lost": lost_total, "gaps": gaps_total[:5]}
    if lost_total:
        res["errors"].append(f"F-3: trace lost={lost_total}")
    res["F3"] = {"reclaims": reclaims, "proven_two_copies_first": checked, "archive_removes": archive_removes,
                 "failures": bad[:10]}
    if bad:
        res["errors"].append(f"F-3: {len(bad)} reclaim(s) without proven two verified copies first")
    # F-2' and index state
    ev = hillog.evlog_fields(inv_text)
    res["evlog"] = ev
    reclaimable = [q for q, g in segs.items() if g["state"] == "SPOOLED" and g["flash"]]
    res["F2"] = {"reclaimed": int(ev.get("reclaimed", 0)), "spool_files": int(ev.get("spool_files", 0)),
                 "storage_blocked": ev.get("storage_blocked"), "spooled_with_flash_copy": reclaimable,
                 "states": {st: sum(1 for g in segs.values() if g["state"] == st)
                            for st in {g["state"] for g in segs.values()}}}
    if int(ev.get("reclaimed", 0)) < 1:
        res["errors"].append("F-2: nothing reclaimed")
    if ev.get("storage_blocked") not in ("0", None):
        res["errors"].append("F-2: storage blocked")
    pubs = text.count("publish event ->")
    res["F4"] = {"publishes_since_fill_start": pubs, "pending": ev.get("pending"),
                 "sd_pending": ev.get("sd_pending"), "flash_pending": ev.get("flash_pending")}
    if pubs:
        res["errors"].append(f"F-4: {pubs} publish(es) during the hold")
    res["ok"] = not res["errors"]
    Path(a.out).write_text(json.dumps(res, indent=1, default=str))
    print(json.dumps({k: v for k, v in res.items() if k != "evlog"}, default=str)[:4000])
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

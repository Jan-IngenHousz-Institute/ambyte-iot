"""Contract §2.1 steps 3-4: the first-HIL-boot SD baseline.

    python p3_check.py --capture cap-P3.log --pre pre_flash_manifest.json --image flash-A.bin \
        [--warehouse-sample 200] --out p3.json

Step 3: the last two complete `sd_inv /sdcard lines` snapshots (S1, S2) must
be identical for every file outside /sdcard/logs (path, size, sha256, crc32,
per-line hashes). Their union is SD_BASE.
Step 4 (identity continuity): every SD_BASE record line whose id is also in
PRE_FLASH (the pre-update backup) has the identical line sha256; each id maps
to one distinct line hash (identical duplicates counted); a seeded sample of
those overlapping ids is matched against open_jii_dev raw_data by the
backup line's content (JSON-semantic sample equality).
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hillog  # noqa: E402

RECORD_DIRS = ("/sdcard/events", "/sdcard/evq", "/sdcard/archive")


def comparable(inv: dict) -> dict:
    files = {p: v for p, v in inv["files"].items() if not p.startswith("/sdcard/logs")}
    lines = {}
    for ln in inv["lines"]:
        if ln["path"].startswith("/sdcard/logs"):
            continue
        lines[(ln["path"], ln["line"])] = (ln["id"], ln["sha256"], ln["bytes"])
    return {"files": files, "lines": lines, "torn": sorted((t["path"], t["line"], t["bytes"]) for t in inv["torn"])}


def backup_lines(img: bytes, pre: dict, ids: set[int]) -> dict[int, bytes]:
    """Content of selected PRE_FLASH lines, read from the private image."""
    import lfs_manifest
    parts = {p["label"]: p for p in lfs_manifest.flashimg.parse_pt(img)}
    st = parts["storage"]
    fs = lfs_manifest.mount(img, st["offset"], st["size"])
    want: dict[int, bytes] = {}
    by_seq: dict[int, list[dict]] = {}
    for ln in pre["storage"]["lines"]:
        if ln.get("id") in ids:
            by_seq.setdefault(ln["seq"], []).append(ln)
    for seq, lns in by_seq.items():
        with fs.open(f"/events/ev-{seq:06d}.log", "rb") as f:
            data = f.read()
        for ln in lns:
            want[ln["id"]] = data[ln["off"]:ln["off"] + ln["bytes"]]
    return want


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--pre", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--warehouse-sample", type=int, default=200)
    ap.add_argument("--min-id", type=int, default=66903, help="first bench id in the dev experiment (2026-09-06 22:10 UTC)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    text = Path(a.capture).read_text(errors="replace")
    invs = [x for x in hillog.sd_inventories(text) if x["dir"] == "/sdcard" and x["complete"]]
    res: dict = {"errors": []}
    if len(invs) < 2:
        res["errors"].append(f"need two complete /sdcard inventories, have {len(invs)}")
    else:
        s1, s2 = invs[-2], invs[-1]
        res["s1_us"], res["s2_us"] = s1["us"], s2["us"]
        res["gap_s"] = (s2["us"] - s1["us"]) / 1e6
        c1, c2 = comparable(s1), comparable(s2)
        res["files"] = len(c1["files"])
        res["record_lines"] = sum(1 for k in c1["lines"] if k[0].startswith(RECORD_DIRS))
        if res["gap_s"] < 120:
            res["errors"].append(f"S1/S2 only {res['gap_s']:.0f} s apart")
        if c1 != c2:
            df = [p for p in set(c1["files"]) | set(c2["files"]) if c1["files"].get(p) != c2["files"].get(p)][:10]
            res["errors"].append(f"S1 != S2: files differing {df}")
        res["logs"] = {p: (s1["names_only"].get(p), s2["names_only"].get(p))
                       for p in sorted(set(s1["names_only"]) | set(s2["names_only"]))}
        # continuity
        pre = json.loads(Path(a.pre).read_text())
        pre_sha = {ln["id"]: ln["sha256"] for ln in pre["storage"]["lines"] if ln.get("id") is not None}
        sd_by_id: dict[int, set] = {}
        occ: dict[int, int] = {}
        for (path, _), (i, h, _) in c1["lines"].items():
            if not path.startswith(RECORD_DIRS):
                continue
            sd_by_id.setdefault(i, set()).add(h)
            occ[i] = occ.get(i, 0) + 1
        multi = {i: sorted(s) for i, s in sd_by_id.items() if len(s) > 1}
        if multi:
            res["errors"].append(f"SD_BASE id(s) with distinct line hashes: {list(multi.items())[:5]}")
        overlap = sorted(set(sd_by_id) & set(pre_sha))
        mism = [i for i in overlap if pre_sha[i] not in sd_by_id[i]]
        res["continuity"] = {"sd_ids": len(sd_by_id), "pre_ids": len(pre_sha), "overlap": len(overlap),
                             "overlap_mismatch": mism[:20], "identical_duplicate_ids": sum(1 for v in occ.values() if v > 1),
                             "sd_id_range": [min(sd_by_id), max(sd_by_id)] if sd_by_id else None}
        if mism:
            res["errors"].append(f"continuity: {len(mism)} overlapping id(s) differ between backup and SD")
        # warehouse sample (contract §2.1 step 4): seeded random archived ids
        # >= the first dev-experiment id, compared by content (the archive's own
        # bytes from `sd_inv /sdcard/archive full`) JSON-semantically.
        full = {}
        for inv in hillog.sd_inventories(text):
            if inv["dir"] == "/sdcard/archive" and inv["complete"] and inv["full"]:
                full = inv["full"]
        arch_ids = {}
        for (path, line), (i, h, _) in c1["lines"].items():
            if path.startswith("/sdcard/archive") and i >= a.min_id and (path, line) in full:
                if hashlib.sha256(full[(path, line)]).hexdigest() == h:
                    arch_ids[i] = full[(path, line)]
        rng = random.Random(20260926)
        pool = sorted(arch_ids)
        sample = sorted(rng.sample(pool, min(a.warehouse_sample, len(pool))))
        res["continuity"]["warehouse_sample"] = len(sample)
        res["continuity"]["sample_pool"] = len(pool)
        if sample:
            import reconcile_warehouse as rw
            rows = rw.parse_rows(rw.fetch_ids(sample, "2026-09-06"))
            ok, missing, bad = 0, [], []
            for i in sample:
                rs = rows.get(i, [])
                if not rs:
                    missing.append(i)
                    continue
                if any(not rw.check_real(r, arch_ids[i]) for r in rs):
                    ok += 1
                else:
                    bad.append({"id": i, "why": rw.check_real(rs[0], arch_ids[i])})
            res["continuity"]["warehouse"] = {"matched": ok, "missing": missing, "mismatched": bad[:10]}
            if missing or bad:
                res["errors"].append(f"continuity warehouse: {len(missing)} missing, {len(bad)} mismatched")
        elif a.warehouse_sample:
            res["errors"].append("continuity warehouse: no archive content inventory to sample from")
    res["ok"] = not res["errors"]
    Path(a.out).write_text(json.dumps(res, indent=1, default=str))
    print(json.dumps({k: v for k, v in res.items() if k != "logs"}, default=str)[:3000])
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Final SD preservation check (contract C-4, E1 item 6).

    python sd_diff.py --base cap-P3.log --base cap-HIL.log --final cap-HIL.log --final-range A B --out sd.json

Every file outside /sdcard/logs in each baseline (the first complete
`sd_inv /sdcard` of each capture) must exist in the final per-file manifest
with identical size, sha256 and crc32. Files new since the baseline are
listed and classified (synthetic spool primaries/mirrors, archives, other).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hillog  # noqa: E402


def first_full(text: str) -> dict:
    invs = [x for x in hillog.sd_inventories(text) if x["dir"] == "/sdcard" and x["complete"]]
    return invs[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", action="append", required=True)
    ap.add_argument("--final", required=True)
    ap.add_argument("--final-range", nargs=2, type=int, required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    raw = Path(a.final).read_bytes()[a.final_range[0]:a.final_range[1]].decode(errors="replace")
    fin = [x for x in hillog.sd_inventories(raw) if x["dir"] == "/sdcard" and x["complete"]][-1]
    res: dict = {"errors": [], "bases": []}
    for b in a.base:
        base = first_full(Path(b).read_text(errors="replace"))
        changed, missing = [], []
        for p, v in base["files"].items():
            if p.startswith("/sdcard/logs"):
                continue
            f = fin["files"].get(p)
            if f is None:
                missing.append(p)
            elif (f["size"], f["sha256"], f["crc32"]) != (v["size"], v["sha256"], v["crc32"]):
                changed.append(p)
        new = sorted(set(fin["files"]) - set(base["files"]))
        cls = {"sd_events_primary": [p for p in new if p.startswith("/sdcard/events/")],
               "sd_evq_mirror": [p for p in new if p.startswith("/sdcard/evq/m-")],
               "sd_evq_other": [p for p in new if p.startswith("/sdcard/evq/") and not p.startswith("/sdcard/evq/m-")],
               "archive": [p for p in new if p.startswith("/sdcard/archive/")],
               "other": [p for p in new if not p.startswith(("/sdcard/events/", "/sdcard/evq/", "/sdcard/archive/"))]}
        res["bases"].append({"capture": b, "base_files": len(base["files"]), "base_us": base["us"],
                             "changed": changed, "missing": missing,
                             "new_counts": {k: len(v) for k, v in cls.items()}, "new_other": cls["other"][:20]})
        if changed or missing:
            res["errors"].append(f"{b}: {len(changed)} changed, {len(missing)} missing pre-existing file(s)")
    res["final_files"] = len(fin["files"])
    res["final_free"] = fin["free"]
    res["ok"] = not res["errors"]
    Path(a.out).write_text(json.dumps(res, indent=1))
    print(json.dumps(res)[:3000])
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

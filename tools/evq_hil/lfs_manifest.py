"""littlefs manifests from a private flash image (contract PRE-3, C-4).

    python lfs_manifest.py IMAGE --out manifest.json

Mounts (read-only, in memory) the event store (`storage`, /evstore) and the
schedule volume (`littlefs`, /littlefs) with the firmware's littlefs geometry
(block 4096, read 128, prog 128, cache 512, lookahead 128, name_max 255 per the on-disk superblock) and
records, per file, size + sha256; per event-store record line, the
measure_id + sha256(line). Output carries hashes only, never payloads.
Needs `littlefs-python` (uv run --with littlefs-python).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import littlefs

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flashimg  # noqa: E402

GEOM = dict(block_size=4096, read_size=128, prog_size=128, cache_size=512, lookahead_size=128, name_max=255)


def mount(img: bytes, off: int, size: int) -> littlefs.LittleFS:
    fs = littlefs.LittleFS(block_count=size // 4096, mount=False, **GEOM)
    fs.context.buffer = bytearray(img[off:off + size])
    fs.mount()
    return fs


def walk(fs: littlefs.LittleFS, top: str = "/") -> list[str]:
    out = []
    for root, dirs, files in fs.walk(top):
        for f in files:
            out.append((root.rstrip("/") + "/" + f) if root != "/" else "/" + f)
    return sorted(out)


def manifest(img: bytes) -> dict:
    parts = {p["label"]: p for p in flashimg.parse_pt(img)}
    res: dict = {}
    st = parts["storage"]
    fs = mount(img, st["offset"], st["size"])
    files, lines = [], []
    for path in walk(fs):
        with fs.open(path, "rb") as f:
            data = f.read()
        files.append({"path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        name = path.rsplit("/", 1)[-1]
        if path.startswith("/events/") and name.startswith("ev-") and name.endswith(".log"):
            seq = int(name[3:-4])
            pos = 0
            n = 0
            while pos < len(data):
                nl = data.find(b"\n", pos)
                if nl < 0:
                    lines.append({"seq": seq, "line": n + 1, "id": None, "torn_bytes": len(data) - pos})
                    break
                ln = data[pos:nl + 1]
                n += 1
                head = ln.split(b"\t", 1)[0]
                lines.append({"seq": seq, "line": n, "off": pos, "id": int(head) if head.isdigit() else None,
                              "sha256": hashlib.sha256(ln).hexdigest(), "bytes": len(ln)})
                pos = nl + 1
    res["storage"] = {"files": files, "lines": lines}
    lf = parts["littlefs"]
    fs2 = mount(img, lf["offset"], lf["size"])
    lfiles = []
    for path in walk(fs2):
        with fs2.open(path, "rb") as f:
            data = f.read()
        lfiles.append({"path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    res["littlefs"] = {"files": lfiles}
    sched = [f for f in lfiles if f["path"] == "/schedule.yaml"]
    res["schedule_sha256"] = sched[0]["sha256"] if sched else None
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    m = manifest(Path(a.image).read_bytes())
    Path(a.out).write_text(json.dumps(m, indent=1))
    st = m["storage"]
    print(json.dumps({"storage_files": len(st["files"]), "record_lines": sum(1 for x in st["lines"] if x.get("id")),
                      "torn": sum(1 for x in st["lines"] if "torn_bytes" in x),
                      "littlefs_files": len(m["littlefs"]["files"]), "schedule_sha256": m["schedule_sha256"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

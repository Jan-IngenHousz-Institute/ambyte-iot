"""Flash image helpers for the HIL preservation checks (contract PRE-2..5,
W-3..W-6): partition-table parse, per-partition sha256, region diff, and the
private dual-read backup through esptool.

    python flashimg.py backup --out-dir DIR            # PRE-2 (0700 dir, 0600 files)
    python flashimg.py backup --out-dir DIR --in-rom --stay-rom
        # replacement amendment r3.3 step 2: inside a romseq ROM session (entered by
        # `romseq.py enter`); both reads are `--after no_reset`, so the app never runs
    python flashimg.py regions IMAGE [--json OUT]      # per-partition sha256
    python flashimg.py diff A B                        # region-by-region equality
    python flashimg.py read OFFSET LEN OUT             # read_flash a region (hard reset after)

Only hashes and sizes ever leave the private directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402  (A2: explicit bench identity, no default board)
PY = "/home/dv/.platformio/penv/bin/python"
ESPTOOL = "/home/dv/.platformio/packages/tool-esptoolpy/esptool.py"
FLASH_SIZE = 16 * 1024 * 1024
PT_OFFSET = 0x8000
REGIONS_FIXED = [("bootloader+gap", 0x0, 0x8000), ("partition_table", 0x8000, 0x1000)]


def esptool(args: list[str], log: Path | None = None) -> str:
    cmd = [PY, ESPTOOL, "--chip", "esp32s3", "--port", bench.port(), *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    if log is not None:
        with open(log, "a") as f:
            f.write(f"$ {' '.join(cmd[2:])}\n{out}\n")
    if r.returncode != 0:
        raise SystemExit(f"esptool failed ({r.returncode}): {' '.join(args)}\n{out[-2000:]}")
    return out


def parse_pt(img: bytes, off: int = PT_OFFSET) -> list[dict]:
    parts = []
    for i in range(0, 0xC00, 32):
        e = img[off + i:off + i + 32]
        if e[:2] != b"\xAA\x50":
            break
        typ, sub, poff, size = struct.unpack_from("<BBII", e, 2)
        label = e[12:28].split(b"\0")[0].decode(errors="replace")
        parts.append({"label": label, "type": typ, "subtype": sub, "offset": poff, "size": size})
    return parts


def regions(img: bytes) -> list[dict]:
    out = []
    for name, off, size in REGIONS_FIXED:
        out.append({"label": name, "offset": hex(off), "size": size,
                    "sha256": hashlib.sha256(img[off:off + size]).hexdigest()})
    for p in parse_pt(img):
        blob = img[p["offset"]:p["offset"] + p["size"]]
        out.append({"label": p["label"], "offset": hex(p["offset"]), "size": p["size"],
                    "type": p["type"], "subtype": p["subtype"], "sha256": hashlib.sha256(blob).hexdigest()})
    return out


def cmd_backup(out_dir: Path, in_rom: bool = False, stay_rom: bool = False) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(out_dir, 0o700)
    a, b, log = out_dir / "flash-A.bin", out_dir / "flash-B.bin", out_dir / "esptool.log"
    if a.exists() or b.exists():
        raise SystemExit(f"{out_dir}: a backup is already there; use a fresh directory (evidence is never overwritten)")
    t0 = time.time()
    # one ROM download session: no app runs between the two reads
    esptool(["--before", "no_reset" if in_rom else "default_reset", "--after", "no_reset",
             "read_flash", "0", hex(FLASH_SIZE), str(a)], log)
    esptool(["--before", "no_reset", "--after", "no_reset" if stay_rom else "hard_reset",
             "read_flash", "0", hex(FLASH_SIZE), str(b)], log)
    for p in (a, b, log):
        os.chmod(p, 0o600)
    da, db = a.read_bytes(), b.read_bytes()
    ha, hb = hashlib.sha256(da).hexdigest(), hashlib.sha256(db).hexdigest()
    meta = {"size_a": len(da), "size_b": len(db), "sha256_a": ha, "sha256_b": hb,
            "identical": ha == hb and len(da) == len(db) == FLASH_SIZE, "seconds": round(time.time() - t0, 1),
            "in_rom": in_rom, "stay_rom": stay_rom,
            "regions": regions(da)}
    (out_dir / "backup-meta.json").write_text(json.dumps(meta, indent=1))
    os.chmod(out_dir / "backup-meta.json", 0o600)
    return meta


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup")
    b.add_argument("--out-dir", required=True)
    b.add_argument("--in-rom", action="store_true", help="already in the ROM loader: no reset before the first read")
    b.add_argument("--stay-rom", action="store_true", help="stay in the ROM loader after the second read")
    r = sub.add_parser("regions")
    r.add_argument("image")
    r.add_argument("--json")
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    rd = sub.add_parser("read")
    rd.add_argument("offset")
    rd.add_argument("length")
    rd.add_argument("out")
    a = ap.parse_args()
    if a.cmd == "backup":
        meta = cmd_backup(Path(a.out_dir), a.in_rom, a.stay_rom)
        print(json.dumps({k: v for k, v in meta.items() if k != "regions"}))
        return 0 if meta["identical"] else 1
    if a.cmd == "regions":
        res = regions(Path(a.image).read_bytes())
        if a.json:
            Path(a.json).write_text(json.dumps(res, indent=1))
        print(json.dumps(res, indent=1))
        return 0
    if a.cmd == "diff":
        ra = {x["label"]: x for x in regions(Path(a.a).read_bytes())}
        rb = {x["label"]: x for x in regions(Path(a.b).read_bytes())}
        res = {k: {"equal": ra[k]["sha256"] == rb.get(k, {}).get("sha256"), "a": ra[k]["sha256"],
                   "b": rb.get(k, {}).get("sha256")} for k in ra}
        print(json.dumps(res, indent=1))
        return 0
    if a.cmd == "read":
        esptool(["--before", "default_reset", "--after", "hard_reset", "read_flash", a.offset, a.length, a.out])
        data = Path(a.out).read_bytes()
        print(json.dumps({"offset": a.offset, "length": len(data), "sha256": hashlib.sha256(data).hexdigest()}))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())

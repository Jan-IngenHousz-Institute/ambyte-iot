#!/usr/bin/env python3
"""REL-period delivery checks (Sprint 2 contract r4 §6.6): REL-ID, REL-EQ, REL-CUR.

    rel_check.py rel-id  --first F --next-id N (--manifest M.json ... | --storage-region R.bin ...)
                         [--rows rows.json ...]
    rel_check.py rel-eq  (--manifest M.json ... --real real.json | --storage-region R.bin ...) --rows rows.json ...
    rel_check.py rel-cur --nvs NVS.bin (--manifest M.json | --storage-region R.bin) --rows rows.json ...

  M.json      lfs_manifest.py output (per stored line: seq, off, id, sha256)
  R.bin       a `flashimg.py read 0x6A0000 0x960000` storage dump (needs littlefs-python;
              gives both the manifest and the private stored lines)
  real.json   {id: base64(stored line)} (private), bound to the manifest by line sha256
  rows.json   reconcile_warehouse.py fetch output (raw warehouse rows, any tag)
  NVS.bin     a `flashimg.py read 0x9000 0x6000` dump or a whole-flash image

Delivery is a warehouse row, never a PUBACK. Output: one JSON object; exit 0 PASS,
1 FAIL, 2 VOID (inputs insufficient).
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import diag_check  # noqa: E402
import reconcile_warehouse as rw  # noqa: E402

PASS, FAIL, VOID = "PASS", "FAIL", "VOID"
EXIT = {PASS: 0, FAIL: 1, VOID: 2}
STORAGE_GEOM_SIZE = 0x960000


def load_rows(paths: list[str] | None) -> dict[int, list[dict]]:
    rows: list[dict] = []
    for p in paths or []:
        rows += json.loads(Path(p).read_text())
    return rw.parse_rows(rows)


def storage_from_region(path: str) -> tuple[list[dict], dict[int, bytes]]:
    """Mount a storage-partition dump read-only (lfs_manifest geometry) -> (manifest
    lines, {id: stored line bytes})."""
    import lfs_manifest  # needs littlefs-python: uv run --with littlefs-python
    data = Path(path).read_bytes()
    fs = lfs_manifest.mount(data, 0, len(data))
    lines, raw = [], {}
    for p in lfs_manifest.walk(fs):
        name = p.rsplit("/", 1)[-1]
        if not (p.startswith("/events/") and name.startswith("ev-") and name.endswith(".log")):
            continue
        with fs.open(p, "rb") as f:
            blob = f.read()
        seq, pos = int(name[3:-4]), 0
        while pos < len(blob):
            nl = blob.find(b"\n", pos)
            if nl < 0:
                break
            ln = blob[pos:nl + 1]
            head = ln.split(b"\t", 1)[0]
            if head.isdigit():
                lines.append({"seq": seq, "off": pos, "id": int(head), "sha256": hashlib.sha256(ln).hexdigest()})
                raw[int(head)] = ln
            pos = nl + 1
    return lines, raw


def manifest_lines(paths: list[str] | None) -> list[dict]:
    out = []
    for p in paths or []:
        m = json.loads(Path(p).read_text())
        out += [x for x in m["storage"]["lines"] if x.get("id") is not None]
    return out


def rel_id(first: int, next_id: int, lines: list[dict], rows: dict[int, list[dict]]) -> dict:
    """Every id in [first, next_id) is in a REL dump's stored lines or in a warehouse row
    of the bench client (event or telemetry). A gap FAILs (refused_* must be 0)."""
    stored = {x["id"] for x in lines}
    missing = [i for i in range(first, next_id) if i not in stored and i not in rows]
    return {"status": PASS if not missing and next_id > first else (VOID if next_id <= first else FAIL),
            "range": [first, next_id], "ids": max(0, next_id - first), "in_dump": len(stored & set(range(first, next_id))),
            "missing": missing[:200], "missing_count": len(missing)}


def rel_eq(lines: list[dict], real: dict[int, bytes], rows: dict[int, list[dict]]) -> dict:
    """Every stored line of every REL dump has a JSON-semantically equal warehouse row
    with a matching timestamp (reconcile_warehouse.check_real). The private line must
    hash to the manifest's sha, so the check is bound to the dump."""
    reasons, missing, failed, passed = [], [], {}, 0
    for x in lines:
        i = x["id"]
        ln = real.get(i)
        if ln is None:
            reasons.append(f"id {i}: stored line bytes not supplied")
            continue
        if hashlib.sha256(ln).hexdigest() != x["sha256"]:
            reasons.append(f"id {i}: supplied line does not hash to the dump manifest")
            continue
        rs = rows.get(i, [])
        if not rs:
            missing.append(i)
            continue
        errs = [rw.check_real(r, ln) for r in rs]
        if any(not e for e in errs):
            passed += 1
        else:
            failed[i] = errs[0]
    status = FAIL if (missing or failed or any("hash" in r for r in reasons)) else (VOID if reasons else PASS)
    return {"status": status, "lines": len(lines), "passed": passed, "missing": missing[:200],
            "missing_count": len(missing), "failed": dict(list(failed.items())[:50]), "reasons": reasons[:50]}


def nvs_cursor(path: str) -> dict:
    region = diag_check.nvs_region(Path(path).read_bytes())
    ents = diag_check.nvs_entries(region)
    idx = diag_check._ns_index(ents, "evlog")
    out: dict = {}
    for e in ents:
        if e.state == "Written" and e.metadata["namespace"] == idx and e.key in ("rd_seq", "rd_off") and e.data \
                and e.metadata["type"] == "uint32_t":
            out[e.key] = int(e.data["value"])
    blob, _notes = diag_check.find_blob(region, "evlog", "cur")
    if blob is not None and len(blob) >= 12:
        s, o, c = struct.unpack_from("<III", blob, 0)
        out["cur"] = {"seq": s, "off": o, "crc": c}
    return out


def rel_cur(cursor: dict, lines: list[dict], rows: dict[int, list[dict]]) -> dict:
    """The NVS cursor (the `cur` blob and the legacy rd_seq/rd_off, each checked) is never
    beyond an undelivered stored id: every stored line behind a cursor position has a
    warehouse row."""
    pos = []
    if "cur" in cursor:
        pos.append(("cur", cursor["cur"]["seq"], cursor["cur"]["off"]))
    if "rd_seq" in cursor:
        pos.append(("legacy", cursor["rd_seq"], cursor.get("rd_off", 0)))
    if not pos:
        return {"status": VOID, "reasons": ["no event_log cursor in NVS"], "cursor": cursor}
    reasons = []
    for label, cs, co in pos:
        behind = [x for x in lines if (x["seq"], x["off"]) < (cs, co)]
        undelivered = sorted(x["id"] for x in behind if x["id"] not in rows)
        if undelivered:
            reasons.append(f"{label} cursor {cs}:{co} is beyond {len(undelivered)} undelivered stored id(s), "
                           f"first {undelivered[:5]}")
    return {"status": FAIL if reasons else PASS, "cursor": cursor, "reasons": reasons}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="rel_check.py", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("rel-id", "rel-eq", "rel-cur"):
        p = sub.add_parser(name)
        p.add_argument("--manifest", nargs="+")
        p.add_argument("--storage-region", nargs="+")
        p.add_argument("--rows", nargs="+", required=name != "rel-id")
        if name == "rel-id":
            p.add_argument("--first", type=int, required=True)
            p.add_argument("--next-id", type=int, required=True)
        if name == "rel-eq":
            p.add_argument("--real", help="JSON {id: base64 stored line} (private)")
        if name == "rel-cur":
            p.add_argument("--nvs", required=True)
    a = ap.parse_args(argv)
    if not a.manifest and not a.storage_region:
        ap.error("--manifest or --storage-region is required")
    lines = manifest_lines(a.manifest)
    real: dict[int, bytes] = {}
    for r in a.storage_region or []:
        ls, raw = storage_from_region(r)
        lines += ls
        real.update(raw)
    if getattr(a, "real", None):
        real.update({int(k): base64.b64decode(v) for k, v in json.loads(Path(a.real).read_text()).items()})
    rows = load_rows(a.rows)
    if a.cmd == "rel-id":
        out = rel_id(a.first, a.next_id, lines, rows)
    elif a.cmd == "rel-eq":
        out = rel_eq(lines, real, rows)
    else:
        out = rel_cur(nvs_cursor(a.nvs), lines, rows)
    print(json.dumps(out, indent=1, default=str))
    return EXIT[out["status"]]


if __name__ == "__main__":
    sys.exit(main())

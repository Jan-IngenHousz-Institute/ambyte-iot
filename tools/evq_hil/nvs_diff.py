"""NVS key inventory from a private flash image (contract PRE-4, W-5), via
ESP-IDF's own parser (components/nvs_flash/nvs_partition_tool). Values are
never emitted: each written entry becomes (namespace, key, type, sha256 of
its raw value bytes). Diff two images to list changed keys.

    python nvs_diff.py inv IMAGE --out keys.json
    python nvs_diff.py diff A B
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flashimg  # noqa: E402

IDF = Path("/home/dv/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_tool")
sys.path.insert(0, str(IDF))
import nvs_parser  # type: ignore # noqa: E402


def inventory(img: bytes) -> list[dict]:
    parts = {p["label"]: p for p in flashimg.parse_pt(img)}
    p = parts["nvs"]
    nvs = nvs_parser.NVS_Partition("nvs", bytearray(img[p["offset"]:p["offset"] + p["size"]]))
    ns_names = {0: "<ns-table>"}
    entries = []
    for page in nvs.pages:
        for e in page.entries:
            if e.state != "Written":
                continue
            if e.metadata["namespace"] == 0 and e.metadata["type"] == "uint8_t" and e.data:
                ns_names[int(e.data["value"])] = e.key
    for page in nvs.pages:
        for e in page.entries:
            if e.state != "Written" or e.metadata["namespace"] == 0:
                continue
            if e.key is None:
                continue
            # data field (inline value, or size/crc of a multi-entry item) + every child entry
            vbytes = bytes(e.raw[24:32]) + b"".join(bytes(c.raw) for c in e.children)
            entries.append({"ns": ns_names.get(e.metadata["namespace"], str(e.metadata["namespace"])),
                            "key": e.key, "type": e.metadata["type"],
                            "value_sha256": hashlib.sha256(vbytes).hexdigest()})
    entries.sort(key=lambda x: (x["ns"], x["key"]))
    return entries


def diff(a: list[dict], b: list[dict]) -> dict:
    ka = {(x["ns"], x["key"]): x for x in a}
    kb = {(x["ns"], x["key"]): x for x in b}
    return {"added": sorted(f"{n}:{k}" for n, k in kb.keys() - ka.keys()),
            "removed": sorted(f"{n}:{k}" for n, k in ka.keys() - kb.keys()),
            "changed": sorted(f"{n}:{k}" for (n, k) in ka.keys() & kb.keys()
                              if ka[(n, k)]["value_sha256"] != kb[(n, k)]["value_sha256"] or
                              ka[(n, k)]["type"] != kb[(n, k)]["type"]),
            "namespaces_a": sorted({x["ns"] for x in a}), "namespaces_b": sorted({x["ns"] for x in b})}


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("inv")
    i.add_argument("image")
    i.add_argument("--out", required=True)
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    a = ap.parse_args()
    if a.cmd == "inv":
        inv = inventory(Path(a.image).read_bytes())
        Path(a.out).write_text(json.dumps(inv, indent=1))
        print(json.dumps({"entries": len(inv), "namespaces": sorted({x["ns"] for x in inv})}))
        return 0
    print(json.dumps(diff(inventory(Path(a.a).read_bytes()), inventory(Path(a.b).read_bytes())), indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


def evlog_cursor(img: bytes) -> dict:
    """Integer event_log cursor keys (not secret): rd_seq, rd_off, cur blob."""
    parts = {p["label"]: p for p in flashimg.parse_pt(img)}
    p = parts["nvs"]
    nvs = nvs_parser.NVS_Partition("nvs", bytearray(img[p["offset"]:p["offset"] + p["size"]]))
    ns_idx = None
    for page in nvs.pages:
        for e in page.entries:
            if e.state == "Written" and e.metadata["namespace"] == 0 and e.key == "evlog" and e.data:
                ns_idx = int(e.data["value"])
    out: dict = {}
    for page in nvs.pages:
        for e in page.entries:
            if e.state != "Written" or e.metadata["namespace"] != ns_idx or e.key is None:
                continue
            if e.key in ("rd_seq", "rd_off", "nid") and e.data:
                out[e.key] = int(e.data["value"])
            elif e.key == "cur":
                raw = b"".join(bytes(c.raw) for c in e.children)
                import struct
                if len(raw) >= 12:
                    s, o, c = struct.unpack_from("<III", raw, 0)
                    out["cur"] = {"seq": s, "off": o, "crc": c}
    return out

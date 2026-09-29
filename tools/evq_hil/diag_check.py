#!/usr/bin/env python3
"""sd_diag decoder and DG-1..5 checks (Sprint 2 contract r4 H6, §6.5).

    diag_check.py nvs IMAGE_OR_NVS_REGION [--offset 0x9000 --size 0x6000] [--expect any|present|absent]
    diag_check.py compare --live CAPTURE --nvs IMAGE [--live-gen N]          (DG-4)
    diag_check.py dg1 --live CAPTURE [--floor IMAGE] [--first-boot]
    diag_check.py dg2 --capture CAP [--from N --to M] --before B --after A [--explained W.OP=N ...]
    diag_check.py dg3 --before B --after A [--reset-reason N]
    diag_check.py dg5 --before B --after A --refs N --first-id I --last-id J

B / A / --live accept either a JSON file holding the sd_diag object or a console
capture (the last `sd_diag={...}` line of `evlog` is used). Output: one JSON
object; exit 0 PASS, 1 FAIL, 2 VOID/INCOMPLETE (evidence missing).

The NVS blob `sd_diag/snap` is the raw `sd_diag_block_t` (components/sd_card/
sd_diag.h) written by nvs_set_blob. Its layout below is the ESP32-S3 (xtensa,
little-endian, natural alignment: int64 aligned to 8) C layout; the offsets are
spelled out rather than trusted to struct's own packing, and
tests/test_diag_check.py compiles sd_diag.h with the host C compiler (same
alignment rules for these types) to cross-check sizeof/offsetof. The contract's
"256-byte layout size" is the RTC budget the struct must fit (_Static_assert in
sd_diag_core.c); the blob length must equal sizeof(sd_diag_block_t) = 200 exactly -
that is what sd_diag.c writes and what its floor reader accepts.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import flashimg  # noqa: E402

IDF_NVS_TOOL = Path("/home/dv/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_tool")

PASS, FAIL, VOID = "PASS", "FAIL", "VOID"
EXIT = {PASS: 0, FAIL: 1, VOID: 2}

MAGIC = 0x53444447            # 'SDDG'
VERSION = 1
WRITERS = ("evlog", "sdlog", "ambit_ota", "ambit_flash")
OPS = ("open", "write", "flush", "fsync", "close", "truncate", "rename", "remove", "mkdir", "stat", "verify", "read")
REFUSALS = ("full", "media", "too_large", "unavailable")
NW, NOP, NREF = len(WRITERS), len(OPS), len(REFUSALS)

# (field, offset, struct code) - sd_diag_block_t on ESP32-S3.
LAYOUT = [
    ("magic", 0, "I"), ("version", 4, "H"), ("size", 6, "H"), ("epoch", 8, "I"), ("boot_seq", 12, "I"),
    ("gen", 16, "I"), ("exact", 20, "B"), ("floor_pending", 21, "B"),          # pad[2] @22
    ("cnt", 24, f"{NW * NOP}H"),                                               # uint16_t[4][12] -> 120
    ("last.writer", 120, "B"), ("last.op", 121, "B"), ("last.err", 122, "h"),
    ("last.uptime_ms", 124, "I"), ("last.boot_seq", 128, "I"),                 # struct last: 12 B
    ("refused", 132, f"{NREF}I"),                                              # -> 148, pad to 152
    ("ref_first_id", 152, "q"), ("ref_last_id", 160, "q"),
    ("ref_last.reason", 168, "B"), ("ref_last.blocked", 169, "B"), ("ref_last.sd_state", 170, "B"),
    ("ref_last.err", 172, "h"), ("ref_last.uptime_ms", 176, "I"), ("ref_last.wall_ms", 184, "q"),
    ("crc", 192, "I"),
]
CRC_OFF = 192                 # offsetof(sd_diag_block_t, crc)
SIZE = 200                    # sizeof: 196 rounded up to the struct's 8-byte alignment


def sd_diag_crc(b: bytes) -> int:
    """sd_diag_crc: reflected CRC-32 (poly 0xEDB88320), init 0xFFFFFFFF, final ~, over
    every byte before `crc` (bit-for-bit the C loop; equals zlib.crc32)."""
    c = 0xFFFFFFFF
    for x in b[:CRC_OFF]:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1) & 0xFFFFFFFF))
    return ~c & 0xFFFFFFFF


def decode_block(b: bytes) -> dict:
    """Validate (length, magic, version, size, CRC) then decode. Never raises."""
    out: dict = {"length": len(b), "valid": False, "reasons": []}
    if len(b) != SIZE:
        out["reasons"].append(f"blob length {len(b)} != sizeof(sd_diag_block_t) {SIZE}")
        return out
    raw = {}
    for name, off, code in LAYOUT:
        v = struct.unpack_from("<" + code, b, off)
        raw[name] = v if len(v) > 1 else v[0]
    if raw["magic"] != MAGIC:
        out["reasons"].append(f"magic 0x{raw['magic']:08x} != 0x{MAGIC:08x}")
    if raw["version"] != VERSION:
        out["reasons"].append(f"version {raw['version']} != {VERSION}")
    if raw["size"] != SIZE:
        out["reasons"].append(f"size field {raw['size']} != {SIZE}")
    crc = sd_diag_crc(b)
    if raw["crc"] != crc:
        out["reasons"].append(f"crc 0x{raw['crc']:08x} != computed 0x{crc:08x}")
    out["valid"] = not out["reasons"]
    cnt = raw["cnt"]
    faults = {f"{WRITERS[w]}.{OPS[o]}": cnt[w * NOP + o] for w in range(NW) for o in range(NOP) if cnt[w * NOP + o]}
    last = None
    if raw["last.uptime_ms"] != 0 or raw["last.err"] != 0:
        last = {"w": WRITERS[raw["last.writer"]] if raw["last.writer"] < NW else "?",
                "op": OPS[raw["last.op"]] if raw["last.op"] < NOP else "?",
                "errno": raw["last.err"], "uptime_ms": raw["last.uptime_ms"], "boot": raw["last.boot_seq"]}
    ref = dict(zip(REFUSALS, raw["refused"]))
    ref.update(first_id=raw["ref_first_id"], last_id=raw["ref_last_id"])
    if sum(raw["refused"]):
        ref["last"] = {"reason": REFUSALS[raw["ref_last.reason"]] if raw["ref_last.reason"] < NREF else "?",
                       "errno": raw["ref_last.err"], "blocked": raw["ref_last.blocked"],
                       "sd_state": raw["ref_last.sd_state"], "wall_ms": raw["ref_last.wall_ms"],
                       "uptime_ms": raw["ref_last.uptime_ms"]}
    out["decoded"] = {"gen": raw["gen"], "exact": bool(raw["exact"]), "epoch": raw["epoch"],
                      "boot": raw["boot_seq"], "floor_pending": raw["floor_pending"], "faults": faults,
                      "last": last, "refused": ref}
    return out


def encode_block(gen=1, epoch=1, boot=1, exact=0, floor_pending=0, cnt=None, last=(0, 0, 0, 0, 0),
                 refused=(0, 0, 0, 0), ref_first=0, ref_last=0, ref_last_rec=(0, 0, 0, 0, 0, 0),
                 magic=MAGIC, version=VERSION, size=SIZE, crc=None) -> bytes:
    """Fixture builder: the exact C layout; crc None = the correct CRC."""
    b = bytearray(SIZE)
    struct.pack_into("<IHHIIIBB", b, 0, magic, version, size, epoch, boot, gen, exact, floor_pending)
    struct.pack_into(f"<{NW * NOP}H", b, 24, *(cnt or [0] * (NW * NOP)))
    struct.pack_into("<BBhII", b, 120, *last)
    struct.pack_into(f"<{NREF}I", b, 132, *refused)
    struct.pack_into("<qq", b, 152, ref_first, ref_last)
    reason, blocked, sd_state, err, uptime, wall = ref_last_rec
    struct.pack_into("<BBBBhhI", b, 168, reason, blocked, sd_state, 0, err, 0, uptime)
    struct.pack_into("<q", b, 184, wall)
    struct.pack_into("<I", b, CRC_OFF, sd_diag_crc(bytes(b)) if crc is None else crc)
    return bytes(b)


# --------------------------------------------------------------------------- NVS

def _nvs_parser():
    sys.path.insert(0, str(IDF_NVS_TOOL))
    import nvs_parser  # type: ignore
    return nvs_parser


def nvs_region(data: bytes, offset: int = 0x9000, size: int = 0x6000) -> bytes:
    """A whole-flash image (partition table at 0x8000) -> its `nvs` partition; a file of
    exactly `size` bytes -> itself (a `flashimg.py read 0x9000 0x6000` dump); else
    data[offset:offset+size]."""
    if len(data) == size:
        return data
    if len(data) >= 0x9000:
        parts = {p["label"]: p for p in flashimg.parse_pt(data)}
        if "nvs" in parts:
            p = parts["nvs"]
            return data[p["offset"]:p["offset"] + p["size"]]
    if len(data) < offset + size:
        raise ValueError(f"input of {len(data)} B holds no NVS region at 0x{offset:x}+0x{size:x}")
    return data[offset:offset + size]


def nvs_entries(region: bytes) -> list:
    np_ = _nvs_parser()
    part = np_.NVS_Partition("nvs", bytearray(region))
    return [e for page in part.pages for e in page.entries]


def _ns_index(entries, ns: str) -> int | None:
    idx = None
    for e in entries:
        if e.state == "Written" and e.metadata["namespace"] == 0 and e.key == ns and e.data:
            idx = int(e.data["value"])
    return idx


def find_blob(region: bytes, ns: str = "sd_diag", key: str = "snap") -> tuple[bytes | None, list[str]]:
    """Reassemble a blob (NVS v2: blob_index + blob_data chunks; v1: one `blob` item)."""
    notes: list[str] = []
    entries = nvs_entries(region)
    idx = _ns_index(entries, ns)
    if idx is None:
        return None, [f"namespace {ns!r} absent"]
    mine = [e for e in entries if e.state == "Written" and e.metadata["namespace"] == idx and e.key == key]
    index = [e for e in mine if e.metadata["type"] == "blob_index"]
    if index:
        if len(index) > 1:
            notes.append(f"{len(index)} written blob_index entries (last taken)")
        ix = index[-1].data
        start, count, total = ix["chunk_start"], ix["chunk_count"], ix["size"]
        buf = b""
        for ci in range(start, start + count):
            ch = [e for e in mine if e.metadata["type"] == "blob_data" and e.metadata["chunk_index"] == ci]
            if not ch:
                return None, notes + [f"blob chunk {ci} missing"]
            e = ch[-1]
            data = b"".join(bytes(c.raw) for c in e.children)[:e.data["size"]]
            if e.metadata["crc"]["data_computed"] != e.metadata["crc"]["data_original"]:
                notes.append(f"chunk {ci} data CRC mismatch")
            buf += data
        if len(buf) != total:
            notes.append(f"reassembled {len(buf)} B != blob_index size {total}")
        return buf, notes
    legacy = [e for e in mine if e.metadata["type"] == "blob"]
    if legacy:
        e = legacy[-1]
        return b"".join(bytes(c.raw) for c in e.children)[:e.data["size"]], notes + ["legacy v1 blob"]
    return None, notes + [f"key {ns}/{key} absent"]


def cmd_nvs(path: str, offset: int, size: int, expect: str) -> dict:
    region = nvs_region(Path(path).read_bytes(), offset, size)
    blob, notes = find_blob(region)
    out: dict = {"input": path, "present": blob is not None, "notes": notes}
    if blob is None:
        out["status"] = FAIL if expect == "present" else PASS
        return out
    dec = decode_block(blob)
    out.update(dec)
    if expect == "absent":
        out["status"] = FAIL
        out.setdefault("reasons", []).append("sd_diag/snap present but expected absent")
    else:
        out["status"] = PASS if dec["valid"] else FAIL
    return out


# --------------------------------------------------------------------------- live JSON

LIVE_RE = re.compile(r"sd_diag=(\{.*\})\s*$")


def load_diag(src: str) -> dict:
    """A JSON file holding the sd_diag object (optionally under key "sd_diag"), or a
    capture whose last `sd_diag={...}` line (evlog) is taken."""
    text = Path(src).read_text(errors="replace")
    try:
        obj = json.loads(text)
        return obj.get("sd_diag", obj) if isinstance(obj, dict) else obj
    except json.JSONDecodeError:
        pass
    found = None
    for ln in text.splitlines():
        m = LIVE_RE.search(ln.replace("\r", ""))
        if m:
            found = json.loads(m.group(1))
    if found is None:
        raise SystemExit(f"{src}: no sd_diag JSON found")
    return found


def compare(live: dict, snap: dict, live_gen: int | None) -> dict:
    """DG-4: the NVS snapshot == the live block (counters, last, refusals, epoch, boot,
    exact) and its gen == the last live gen before the forced persist."""
    reasons = []
    for k in ("exact", "epoch", "boot"):
        if live.get(k) != snap.get(k):
            reasons.append(f"{k}: live {live.get(k)!r} != nvs {snap.get(k)!r}")
    lf = {k: v for k, v in (live.get("faults") or {}).items() if v}
    if lf != snap["faults"]:
        reasons.append(f"faults: live {lf} != nvs {snap['faults']}")
    if (live.get("last") or None) != snap["last"]:
        reasons.append(f"last: live {live.get('last')} != nvs {snap['last']}")
    lr = dict(live.get("refused") or {})
    if lr != snap["refused"]:
        reasons.append(f"refused: live {lr} != nvs {snap['refused']}")
    gen = live.get("gen", live_gen)
    out = {"gen_nvs": snap["gen"], "gen_live": gen}
    if gen is None:
        out["gen_check"] = "unavailable (the evlog sd_diag JSON carries no gen; pass --live-gen)"
        status = FAIL if reasons else VOID
    else:
        if int(gen) != snap["gen"]:
            reasons.append(f"gen: live {gen} != nvs {snap['gen']}")
        status = FAIL if reasons else PASS
    return {**out, "status": status, "reasons": reasons}


# --------------------------------------------------------------------------- DG-1/2/3/5

FIRED_RE = re.compile(r"HIL_FAULT fired (.*)$")
RESET_KINDS = ("cpu_reset",)


def fired_lines(path: str, lo: int | None, hi: int | None) -> list[dict]:
    data = Path(path).read_bytes()[lo or 0:hi]
    out = []
    for ln in data.decode(errors="replace").splitlines():
        m = FIRED_RE.search(ln.replace("\r", ""))
        if m:
            kv = dict(t.split("=", 1) for t in m.group(1).split() if "=" in t)
            if "writer" in kv and "op" in kv:
                out.append(kv)
    return out


def dg2(fired: list[dict], before: dict, after: dict, explained: dict[str, int]) -> dict:
    """DG-2a: per (writer, op) the sd_diag count delta == the fired count (voided firings
    included), last.{w,op,errno} == the last firing. Reset-kind firings are listed but
    not expected in the counts (the CPU reset pre-empts the writer's own fault note).
    DG-2b: any other delta is organic and must be explained (--explained W.OP=N,
    from the trace + console failure line); unexplained deltas FAIL."""
    reasons = []
    counted = [f for f in fired if f.get("kind") not in RESET_KINDS]
    want: dict[str, int] = {}
    for f in counted:
        k = f"{f['writer']}.{f['op']}"
        want[k] = want.get(k, 0) + 1
    bf, af = before.get("faults") or {}, after.get("faults") or {}
    rows = {}
    for k in sorted(set(bf) | set(af) | set(want) | set(explained)):
        d = af.get(k, 0) - bf.get(k, 0)
        exp = want.get(k, 0) + explained.get(k, 0)
        rows[k] = {"delta": d, "fired": want.get(k, 0), "explained_organic": explained.get(k, 0)}
        if d != exp:
            reasons.append(f"{k}: delta {d} != fired {want.get(k, 0)} + explained {explained.get(k, 0)}")
    if counted:
        lf = counted[-1]
        err = int(lf.get("errno", 0)) or 5          # a short write reports errno 0; the writer notes EIO
        last = after.get("last") or {}
        if (last.get("w"), last.get("op"), last.get("errno")) != (lf["writer"], lf["op"], err):
            reasons.append(f"last {last} != last firing {lf['writer']}.{lf['op']} errno {err}")
    for k in ("full", "media", "too_large", "unavailable"):
        d = (after.get("refused") or {}).get(k, 0) - (before.get("refused") or {}).get(k, 0)
        if d:
            reasons.append(f"refused.{k} moved by {d} (must stay 0 outside M-2)")
    return {"status": FAIL if reasons else PASS, "per_op": rows,
            "reset_firings": [f for f in fired if f.get("kind") in RESET_KINDS], "reasons": reasons}


def dg1(live: dict, floor: dict | None, first_boot: bool) -> dict:
    """DG-1: exact never true on an image's first boot or after an invalid RTC block;
    no floor -> epoch 1; with a floor -> floor epoch + 1 and every count >= the floor."""
    reasons = []
    if first_boot and live.get("exact"):
        reasons.append("exact=true on the first boot of an image")
    if floor is None:
        if live.get("epoch") != 1:
            reasons.append(f"no floor but epoch {live.get('epoch')} != 1")
    else:
        if live.get("epoch") != floor["epoch"] + 1:
            reasons.append(f"epoch {live.get('epoch')} != floor epoch {floor['epoch']} + 1")
        lf = live.get("faults") or {}
        for k, v in floor["faults"].items():
            if lf.get(k, 0) < v:
                reasons.append(f"{k}: {lf.get(k, 0)} below the floor {v}")
        if live.get("exact"):
            reasons.append("exact=true after a floor merge (a power-on is never proof of continuity)")
    return {"status": FAIL if reasons else PASS, "reasons": reasons}


def dg3(before: dict, after: dict, reset_reason: int | None) -> dict:
    """DG-3 for one CPU reset: RTC path (same epoch) -> boot +1, exact unchanged, counts
    retained; otherwise the floor-merge path (DG-1 applies) - which path is reported."""
    reasons = []
    path = "rtc" if after.get("epoch") == before.get("epoch") else "floor_merge"
    if path == "rtc":
        if after.get("boot") != before.get("boot", 0) + 1:
            reasons.append(f"boot {after.get('boot')} != {before.get('boot')} + 1")
        if bool(after.get("exact")) != bool(before.get("exact")):
            reasons.append("exact changed across an RTC-continuous reset")
        bf, af = before.get("faults") or {}, after.get("faults") or {}
        for k, v in bf.items():
            if af.get(k, 0) < v:
                reasons.append(f"{k}: {af.get(k, 0)} < {v} (counts not retained)")
    elif after.get("exact"):
        reasons.append("floor-merge path but exact=true")
    return {"status": FAIL if reasons else PASS, "path": path, "reset_reason": reset_reason, "reasons": reasons}


def dg5(before: dict, after: dict, refs: int, first_id: int, last_id: int) -> dict:
    reasons = []
    br, ar = before.get("refused") or {}, after.get("refused") or {}
    d = ar.get("full", 0) - br.get("full", 0)
    if d != refs:
        reasons.append(f"refused.full delta {d} != EVQ_REF count {refs}")
    if br.get("first_id", 0) == 0 and ar.get("first_id") != first_id:
        reasons.append(f"first_id {ar.get('first_id')} != first EVQ_REF id {first_id}")
    if ar.get("last_id") != last_id:
        reasons.append(f"last_id {ar.get('last_id')} != last EVQ_REF id {last_id}")
    return {"status": FAIL if reasons else PASS, "reasons": reasons}


# --------------------------------------------------------------------------- CLI

def _emit(obj: dict) -> int:
    print(json.dumps(obj, indent=1, default=str))
    return EXIT.get(obj.get("status"), 1)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="diag_check.py", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("nvs")
    n.add_argument("image")
    n.add_argument("--offset", type=lambda s: int(s, 0), default=0x9000)
    n.add_argument("--size", type=lambda s: int(s, 0), default=0x6000)
    n.add_argument("--expect", choices=("any", "present", "absent"), default="any")
    c = sub.add_parser("compare")
    c.add_argument("--live", required=True)
    c.add_argument("--nvs", required=True)
    c.add_argument("--offset", type=lambda s: int(s, 0), default=0x9000)
    c.add_argument("--size", type=lambda s: int(s, 0), default=0x6000)
    c.add_argument("--live-gen", type=int)
    d1 = sub.add_parser("dg1")
    d1.add_argument("--live", required=True)
    d1.add_argument("--floor", help="NVS image/region holding the pre-boot snapshot")
    d1.add_argument("--first-boot", action="store_true")
    d2 = sub.add_parser("dg2")
    d2.add_argument("--capture", required=True)
    d2.add_argument("--from", dest="lo", type=lambda s: int(s, 0))
    d2.add_argument("--to", dest="hi", type=lambda s: int(s, 0))
    d2.add_argument("--before", required=True)
    d2.add_argument("--after", required=True)
    d2.add_argument("--explained", action="append", default=[], help="W.OP=N organic deltas explained (DG-2b)")
    d3 = sub.add_parser("dg3")
    d3.add_argument("--before", required=True)
    d3.add_argument("--after", required=True)
    d3.add_argument("--reset-reason", type=int)
    d5 = sub.add_parser("dg5")
    d5.add_argument("--before", required=True)
    d5.add_argument("--after", required=True)
    d5.add_argument("--refs", type=int, required=True)
    d5.add_argument("--first-id", type=int, required=True)
    d5.add_argument("--last-id", type=int, required=True)
    a = ap.parse_args(argv)
    if a.cmd == "nvs":
        return _emit(cmd_nvs(a.image, a.offset, a.size, a.expect))
    if a.cmd == "compare":
        blob, notes = find_blob(nvs_region(Path(a.nvs).read_bytes(), a.offset, a.size))
        if blob is None:
            return _emit({"status": FAIL, "reasons": notes})
        dec = decode_block(blob)
        if not dec["valid"]:
            return _emit({"status": FAIL, "reasons": dec["reasons"]})
        return _emit(compare(load_diag(a.live), dec["decoded"], a.live_gen))
    if a.cmd == "dg1":
        floor = None
        if a.floor:
            blob, _n = find_blob(nvs_region(Path(a.floor).read_bytes()))
            dec = decode_block(blob) if blob is not None else None
            floor = dec["decoded"] if dec and dec["valid"] else None
        return _emit(dg1(load_diag(a.live), floor, a.first_boot))
    if a.cmd == "dg2":
        ex = {k: int(v) for k, v in (x.split("=", 1) for x in a.explained)}
        return _emit(dg2(fired_lines(a.capture, a.lo, a.hi), load_diag(a.before), load_diag(a.after), ex))
    if a.cmd == "dg3":
        return _emit(dg3(load_diag(a.before), load_diag(a.after), a.reset_reason))
    return _emit(dg5(load_diag(a.before), load_diag(a.after), a.refs, a.first_id, a.last_id))


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""G-MIG: executable backward-compatibility proof, v1.11.0 <-> candidate event store.

Two host executables (build.py) run the PRODUCTION event_log of each firmware
over one shared device state directory:

  <state>/evstore/        the littlefs /evstore partition (a host directory)
  <state>/sdcard/         the SD card (only when a scenario inserts one)
  <state>/nvs.db          NVS (nvs_model.c); nvs.json is the readable form
  <state>/out/            harness evidence (accepted/acked/probe logs, health)
  <state>/.shim/          media-shim journals, nvs_ops.jsonl (every NVS call)

Oracle, per state:
  corpus     every stored record (id -> exact line sha256): from the drivers'
             accepted logs (synthetic) or from the files themselves (a real
             device dump). It never shrinks: a record may only move between
             flash, SD primary/mirror and SD archive, never disappear.
  must_have  corpus minus records PUBACKed in a real session (acked-*.jsonl);
             for a real dump, the records at/after the dump's v1.11.0 cursor.
  probe      what a firmware actually delivers: the state is COPIED and that
             firmware claims + in-order PUBACKs everything (window 16). The
             original state is never touched by a probe.
  static     the pending list derived from NVS (legacy rd_seq/rd_off, and the
             candidate's `cur` blob reconciled exactly as evq_reconcile_cursor_
             locked does) plus the files, independently of any firmware.

CLI:
  gmig.py scenarios [NAME ...]            run synthetic scenarios, print JSON
  gmig.py check --evstore DIR --nvs FILE  run the migration round trip on a
          [--sdcard DIR]                  REAL device dump (inputs are copied, never modified)
Full results: $TMPDIR/gmig_host/results/<scenario>.json; state dirs: $TMPDIR/gmig_host/runs/.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import importlib.util as _ilu  # noqa: E402

# Loaded by path under a unique name: a bare `import build` resolves to whichever
# `build` module another test (or pypa/build) put in sys.modules first.
_spec = _ilu.spec_from_file_location("gmig_host_build", Path(__file__).resolve().parent / "build.py")
B = _ilu.module_from_spec(_spec)
sys.modules["gmig_host_build"] = B
_spec.loader.exec_module(B)

PARTITION_BYTES = 0x960000          # partitions.csv "storage" (identical at v1.11.0 and the candidate)
SD_BYTES = 256 * 1024 * 1024
TYPES = {"u8": "<B", "i8": "<b", "u16": "<H", "i16": "<h", "u32": "<I", "i32": "<i", "u64": "<Q", "i64": "<q"}
EV_RE = re.compile(r"^ev-(\d+)\.log$")

_EXES: dict | None = None


def exes() -> dict:
    global _EXES
    if _EXES is None:
        _EXES = B.build()
    return _EXES


def runs_root() -> Path:
    p = B.tmp_root() / "runs"
    p.mkdir(parents=True, exist_ok=True)
    return p


# ── NVS: nvs.db <-> JSON ────────────────────────────────────────────────────

def nvs_read(state: Path) -> dict:
    """{ns: {key: {"type", "hex", "value"}}}; namespaces with no keys map to {}."""
    out: dict = {}
    p = state / "nvs.db"
    if not p.exists():
        return out
    for line in p.read_text().splitlines():
        if line.startswith("@ "):
            out.setdefault(line[2:].strip(), {})
            continue
        ns, key, typ, hx = line.split(" ")
        raw = b"" if hx == "-" else bytes.fromhex(hx)
        ent = {"type": typ, "hex": raw.hex()}
        if typ in TYPES:
            ent["value"] = struct.unpack(TYPES[typ], raw)[0]
        elif typ == "str":
            ent["value"] = raw.rstrip(b"\0").decode(errors="replace")
        out.setdefault(ns, {})[key] = ent
    return out


def nvs_write(state: Path, nvs: dict) -> None:
    """Accepts {ns: {key: {"type": t, "value": v} | {"type": t, "hex": h}}}."""
    lines = [f"@ {ns}" for ns in nvs]
    for ns, keys in nvs.items():
        for key, ent in keys.items():
            t = ent["type"]
            if "hex" in ent:
                raw = bytes.fromhex(ent["hex"])
            elif t in TYPES:
                raw = struct.pack(TYPES[t], int(ent["value"]))
            elif t == "str":
                raw = ent["value"].encode() + b"\0"
            else:
                raise ValueError(f"{ns}/{key}: blob needs hex")
            lines.append(f"{ns} {key} {t} {raw.hex() or '-'}")
    (state / "nvs.db").write_text("\n".join(lines) + "\n")


def cursor_nvs(nvs: dict) -> dict:
    ev = nvs.get("evlog", {})
    leg = None
    if "rd_seq" in ev:
        leg = (ev["rd_seq"]["value"], ev.get("rd_off", {}).get("value", 0))
    blob = None
    if "cur" in ev:
        raw = bytes.fromhex(ev["cur"]["hex"])
        if len(raw) == 12:
            seq, off, crc = struct.unpack("<III", raw)
            blob = {"seq": seq, "off": off, "crc_ok": crc == evq_crc32(struct.pack("<II", seq, off), 0x5EC0)}
        else:
            blob = {"len": len(raw), "crc_ok": False}
    return {"legacy": leg, "blob": blob, "nid": ev.get("nid", {}).get("value")}


def evq_crc32(data: bytes, crc: int = 0) -> int:
    return zlib.crc32(data, crc) & 0xFFFFFFFF      # same reflected CRC-32 as evq_index.c


# ── files ───────────────────────────────────────────────────────────────────

def parse_lines(path: Path) -> list[dict]:
    data = path.read_bytes()
    recs, off = [], 0
    while off < len(data):
        nl = data.find(b"\n", off)
        end = len(data) if nl < 0 else nl + 1
        line = data[off:end]
        m = re.match(rb"^(\d+)", line)
        recs.append({"off": off, "len": len(line), "id": int(m.group(1)) if m else None,
                     "sha": hashlib.sha256(line).hexdigest(), "framed": nl >= 0,
                     "valid": nl >= 0 and _valid(line)})
        off = end
    return recs


def _valid(line: bytes) -> bool:
    f = line[:-1].split(b"\t")
    return len(f) >= 9 and f[8][:1] in (b"{", b"[") and f[0].isdigit() and int(f[0]) > 0


def flash_files(state: Path) -> dict[int, Path]:
    d = state / "evstore" / "events"
    out = {}
    if d.is_dir():
        for p in d.iterdir():
            m = EV_RE.match(p.name)
            if m and int(m.group(1)) <= 0xFFFFFFFF:
                out[int(m.group(1))] = p
    return dict(sorted(out.items()))


def record_files(state: Path, sd_root: Path | None = None) -> list[tuple[str, Path]]:
    """Every file that can hold record lines, tagged by role (sd_root: a card
    that is currently pulled out of the state)."""
    out = [("flash", p) for p in flash_files(state).values()]
    q = state / "evstore" / "events" / "quarantine.log"
    if q.exists():
        out.append(("quarantine", q))
    sd = sd_root or state / "sdcard"
    for sub, role in (("events", "sd_primary"), ("evq", "sd_mirror"), ("archive", "sd_archive")):
        d = sd / sub
        if d.is_dir():
            out += [(role, p) for p in sorted(d.iterdir()) if p.is_file() and p.name.endswith(".log")]
    return out


def index_fold(state: Path) -> dict:
    """Replay evstore/evq.idx exactly like evq_index_load (CRC-checked lines)."""
    p = state / "evstore" / "evq.idx"
    res = {"exists": p.exists(), "lines": 0, "bad_lines": 0, "watermark": None, "segs": {}}
    if not p.exists():
        return res
    for raw in p.read_bytes().split(b"\n")[:-1]:
        s = raw.decode(errors="replace")
        i = s.rfind(" *")
        if i < 0 or len(s) - i - 2 != 8 or f"{evq_crc32(s[:i].encode()):08x}" != s[i + 2:]:
            res["bad_lines"] += 1
            continue
        b = s[:i].split(" ")
        k, a = b[0], b[1:]
        segs = res["segs"]
        if k == "S":
            segs[int(a[0])] = {"state": "FLASH", "first": int(a[1]), "last": int(a[2]), "count": int(a[3]),
                               "bytes": int(a[4]), "crc": a[5], "primary": "", "mirror": ""}
        elif k == "P" and int(a[0]) in segs:
            segs[int(a[0])].update(state="SPOOLED", cid=a[1], primary=a[2], mirror=a[3])
        elif k in "ODR" and int(a[0]) in segs:
            segs[int(a[0])]["state"] = {"O": "SD_ONLY", "D": "DELIVERED", "R": "REIMPORT"}[k]
        elif k == "A":
            segs.pop(int(a[0]), None)
        elif k == "W":
            res["watermark"] = [int(a[0]), int(a[1])]
        res["lines"] += 1
    return res


def snapshot(state: Path) -> dict:
    files = {}
    for top in ("evstore", "sdcard"):
        d = state / top
        if d.is_dir():
            for p in sorted(d.rglob("*")):
                if p.is_file():
                    b = p.read_bytes()
                    files[str(p.relative_to(state))] = {"size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
    return {"files": files, "nvs": nvs_read(state)}


def diff(a: dict, b: dict) -> dict:
    fa, fb = a["files"], b["files"]
    out = {"created": sorted(set(fb) - set(fa)), "removed": sorted(set(fa) - set(fb)),
           "modified": sorted(k for k in set(fa) & set(fb) if fa[k] != fb[k]), "nvs": []}
    for ns in sorted(set(a["nvs"]) | set(b["nvs"])):
        ka, kb = a["nvs"].get(ns), b["nvs"].get(ns)
        if ka is None:
            out["nvs"].append({"ns": ns, "change": "namespace created"})
            ka = {}
        kb = kb or {}
        for k in sorted(set(ka) | set(kb)):
            if ka.get(k) != kb.get(k):
                out["nvs"].append({"ns": ns, "key": k, "before": ka.get(k), "after": kb.get(k)})
    return out


# ── running firmware ────────────────────────────────────────────────────────

class FirmwareError(RuntimeError):
    pass


def run(fw: str, state: Path, script: list[str], env: dict | None = None) -> dict:
    exe = exes()["old" if fw == "v1.11.0" else "new"]
    (state / "script.txt").write_text("\n".join(script) + "\n")
    (state / "out").mkdir(exist_ok=True)
    n0 = len(jsonl(state / "out" / "health.jsonl"))
    base = {k: v for k, v in os.environ.items() if k not in ("EVQ_QUIET", "EVQ_VERBOSE")}
    e = {**base, "EVQ_FLASH_BYTES": str(PARTITION_BYTES), "EVQ_SD_BYTES": str(SD_BYTES),
         "GMIG_SEED": "1", "GMIG_SCENARIO": state.name, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
         "UBSAN_OPTIONS": "print_stacktrace=1", **(env or {})}
    r = subprocess.run([str(exe), "script.txt"], cwd=state, capture_output=True, text=True, env=e, timeout=900)
    with open(state / "out" / "runs.log", "a") as f:
        f.write(f"=== {fw}: {script}\nrc={r.returncode}\n{r.stderr[-20000:]}\n")
    if r.returncode != 0 or "ERROR: AddressSanitizer" in r.stderr or "runtime error:" in r.stderr:
        raise FirmwareError(f"{fw} exited {r.returncode} in {state}:\n{r.stderr[-4000:]}")
    hs = jsonl(state / "out" / "health.jsonl")[n0:]
    return {"rc": r.returncode, "stderr_all": r.stderr, "health": {h["label"]: h for h in hs}, "stderr_warnings": [l for l in r.stderr.splitlines() if l.startswith(("W ", "E "))][-40:]}


def jsonl(p: Path) -> list[dict]:
    return [json.loads(l) for l in p.read_text().splitlines() if l.strip()] if p.exists() else []


def health(state: Path, label: str | None = None) -> dict:
    hs = jsonl(state / "out" / "health.jsonl")
    if label:
        hs = [h for h in hs if h.get("label") == label]
    return hs[-1] if hs else {}


def probe(fw: str, state: Path, pre: list[str] | None = None, env: dict | None = None, tag: str = "",
          rounds: int = 0) -> dict:
    """Deliver EVERYTHING the firmware will deliver, on a copy of `state`
    (`rounds` extra keeper-pass + 61 s + deliver cycles, for SD imports that
    wait for flash room)."""
    cp = state.parent / f"{state.name}.probe-{fw}{tag}"
    if cp.exists():
        shutil.rmtree(cp)
    shutil.copytree(state, cp, symlinks=True)
    for f in ("probe.jsonl", "health.jsonl", "events.jsonl"):
        (cp / "out" / f).unlink(missing_ok=True)
    body = ["deliver all probe.jsonl"] + ["service 3", "tick 61000", "deliver all probe.jsonl"] * rounds
    run(fw, cp, [*(pre or []), "health probe-start", *body, "health probe-end"], env)
    got = jsonl(cp / "out" / "probe.jsonl")
    ev = [e for e in jsonl(cp / "out" / "events.jsonl") if e.get("log") == "probe.jsonl"]
    return {"fw": fw, "dir": str(cp), "order": [(g["id"], g["line_sha"]) for g in got],
            "start": health(cp, "probe-start"), "end": health(cp, "probe-end"),
            "end_code": ev[-1]["end_name"] if ev else None}


# ── oracle ──────────────────────────────────────────────────────────────────

def corpus_from_logs(state: Path) -> dict[int, str]:
    c: dict[int, str] = {}
    for a in jsonl(state / "out" / "accepted.jsonl"):
        if a["rc"] == 0:
            if a["id"] in c:
                raise AssertionError(f"id {a['id']} accepted twice (id reuse)")
            c[a["id"]] = a["line_sha"]
    return c


def acked_ids(state: Path) -> set[int]:
    s: set[int] = set()
    for p in sorted((state / "out").glob("acked-*.jsonl")):
        s |= {x["id"] for x in jsonl(p)}
    return s


def locate(state: Path, sd_root: Path | None = None) -> dict[int, list[dict]]:
    loc: dict[int, list[dict]] = {}
    for role, p in record_files(state, sd_root):
        for r in parse_lines(p):
            if r["id"] is not None and r["framed"]:
                m = EV_RE.match(p.name)
                loc.setdefault(r["id"], []).append({"role": role, "file": p.name, "off": r["off"], "sha": r["sha"],
                                                    "seq": int(m.group(1)) if m and role == "flash" else None})
    return loc


def v1110_static(state: Path) -> dict:
    """v1.11.0 evlog_open_locked + claim walk, derived from files and NVS only."""
    ff = flash_files(state)
    cur = cursor_nvs(nvs_read(state))
    mn, mx = (min(ff), max(ff)) if ff else (1, 1)
    cseq, coff = cur["legacy"] if cur["legacy"] else (mn, 0)
    if cseq < mn:
        cseq, coff = mn, 0
    if cseq > mx:
        cseq, coff = mx, 0
    if cseq in ff:
        coff = min(coff, ff[cseq].stat().st_size)
    else:
        coff = min(coff, 0)
    pend, gaps, bad = [], [], []
    for seq in range(cseq, mx + 1):
        if seq not in ff:
            gaps.append(seq)
            continue
        for r in parse_lines(ff[seq]):
            if seq == cseq and r["off"] < coff:
                continue
            if not r["valid"]:
                bad.append({"seq": seq, "off": r["off"]})
                continue
            pend.append((r["id"], r["sha"]))
    return {"cursor": [cseq, coff], "pending": pend, "gaps": gaps, "bad": bad}


def cand_static(state: Path) -> dict:
    """Candidate evq_reconcile_cursor_locked + clamp + walk (flash, else the
    indexed SD copy of an SD_ONLY/SPOOLED segment)."""
    ff = flash_files(state)
    ix = index_fold(state)
    cur = cursor_nvs(nvs_read(state))
    leg, blob = cur["legacy"], cur["blob"] if cur["blob"] and cur["blob"].get("crc_ok") else None
    foreign = False
    if blob is None:
        cseq, coff = leg if leg else (0, 0)
    elif leg is None:
        cseq, coff = blob["seq"], blob["off"]
    else:
        foreign = leg > (blob["seq"], blob["off"])
        cseq, coff = leg if leg != (blob["seq"], blob["off"]) else (blob["seq"], blob["off"])
    segs = {s: v for s, v in ix["segs"].items() if v["state"] != "REIMPORT"}
    mn, mx = (min(ff), max(ff)) if ff else (1, 1)
    tail = mx if not segs or max(segs) < mx else max(segs) + 1
    qmin = min([mn, *segs]) if segs else mn
    if cseq == 0:
        cseq, coff = qmin, 0
    if cseq < qmin:
        cseq, coff = qmin, 0
    if cseq > tail:
        cseq, coff = tail, 0
    pend, gaps, bad, sd_used = [], [], [], []
    for seq in range(cseq, tail + 1):
        path = ff.get(seq)
        if path is None and seq in segs and segs[seq]["primary"]:
            for sub, name in (("events", segs[seq]["primary"]), ("evq", segs[seq]["mirror"])):
                cand = state / "sdcard" / sub / name
                if cand.exists():
                    path = cand
                    sd_used.append(str(cand.relative_to(state)))
                    break
        if path is None:
            if seq != tail:
                gaps.append(seq)
            continue
        for r in parse_lines(path):
            if seq == cseq and r["off"] < coff:
                continue
            if not r["valid"]:
                bad.append({"seq": seq, "off": r["off"]})
                continue
            pend.append((r["id"], r["sha"]))
    return {"cursor": [cseq, coff], "foreign": foreign, "pending": pend, "gaps": gaps, "bad": bad, "sd_used": sd_used}


def positions_before(state: Path, cursor: tuple[int, int]) -> set[int]:
    """Ids of flash records strictly before `cursor` (seq, off)."""
    ids = set()
    for seq, p in flash_files(state).items():
        for r in parse_lines(p):
            if r["id"] is not None and (seq < cursor[0] or (seq == cursor[0] and r["off"] < cursor[1])):
                ids.add(r["id"])
    return ids


def evaluate(state: Path, corpus: dict[int, str], must: set[int], pr: dict, strict: bool) -> dict:
    """All compatibility checks of one firmware's delivery of `state`."""
    chk: dict[str, dict] = {}
    order = pr["order"]
    ids = [i for i, _ in order]
    seen: set[int] = set()
    first_must = []
    for i in ids:
        if i not in seen and i in must:
            first_must.append(i)
        seen.add(i)
    missing = sorted(must - seen)
    wrong = sorted({i for i, s in order if corpus.get(i) != s})
    redelivered = sorted({i for i in ids if i not in must})
    dups = len(ids) - len(seen)
    chk["no_loss"] = {"ok": not missing, "n_missing": len(missing), "missing": _ranges(missing)}
    chk["bytes_exact"] = {"ok": not wrong, "mismatched_ids": wrong[:20]}
    chk["no_phantoms"] = {"ok": seen <= set(corpus), "phantoms": sorted(seen - set(corpus))[:20]}
    dis = next((first_must[k] for k in range(1, len(first_must)) if first_must[k] < first_must[k - 1]), None)
    chk["fifo_order"] = {"ok": dis is None or not strict, "strict": strict, "first_out_of_order_id": dis}
    fwk = pr["end"]
    sk = {k: fwk.get(k, 0) for k in ("skipped", "skipped_unindexed_gap", "quarantined_malformed",
                                      "quarantined_poison", "corrupt_detected", "dropped")}
    chk["no_skip"] = {"ok": all(v == 0 for v in sk.values()), **sk}
    chk["exact_pending"] = {"ok": ids == sorted(must) or not strict, "strict": strict, "delivered": len(ids),
                            "must_have": len(must), "redelivered_acked": len(redelivered),
                            "redelivered_ranges": _ranges(redelivered)[:10], "duplicates_in_probe": dups}
    chk["drained"] = {"ok": pr["end_code"] == "ESP_ERR_NOT_FOUND", "end_code": pr["end_code"]}
    return chk


def _ranges(xs: list[int]) -> list[str]:
    out, xs = [], sorted(xs)
    i = 0
    while i < len(xs):
        j = i
        while j + 1 < len(xs) and xs[j + 1] == xs[j] + 1:
            j += 1
        out.append(str(xs[i]) if i == j else f"{xs[i]}-{xs[j]}")
        i = j + 1
    return out


def corpus_check(state: Path, corpus: dict[int, str], must: set[int], sd_root: Path | None,
                 drops: list[dict]) -> dict:
    """Undelivered records must all exist (exact bytes) somewhere; delivered
    records may leave the device only by an explained path (drops = what each
    step removed, with the firmware's own log reasons)."""
    loc = locate(state, sd_root)
    gone = sorted(i for i, s in corpus.items() if not any(x["sha"] == s for x in loc.get(i, [])))
    gone_must = [i for i in gone if i in must]
    off_flash: dict = {}
    for i, s in corpus.items():
        where = [x for x in loc.get(i, []) if x["sha"] == s]
        if where and not any(x["role"] == "flash" for x in where):
            key = ",".join(sorted({x["role"] for x in where}))
            off_flash[key] = off_flash.get(key, 0) + 1
    unexplained = [d for d in drops if d["unexplained_ids"]]
    ix = index_fold(state)
    return {"ok": not gone_must, "records": len(corpus), "undelivered_missing": _ranges(gone_must),
            "delivered_gone": len(gone) - len(gone_must),
            "delivered_ok": not unexplained, "drops": drops,
            "not_on_flash_by_location": off_flash,
            "index": {"exists": ix["exists"], "lines": ix["lines"], "bad_lines": ix["bad_lines"],
                      "watermark": ix["watermark"], "states": _count(v["state"] for v in ix["segs"].values())}}


def _count(it) -> dict:
    d: dict = {}
    for x in it:
        d[x] = d.get(x, 0) + 1
    return d


def cursor_checks(state: Path, must: set[int]) -> dict:
    cur = cursor_nvs(nvs_read(state))
    out = {"nvs": cur}
    for name, c in (("legacy", cur["legacy"]), ("blob", (cur["blob"]["seq"], cur["blob"]["off"])
                                                  if cur["blob"] and cur["blob"].get("crc_ok") else None)):
        if c is None:
            continue
        passed = positions_before(state, tuple(c)) & must
        out[f"{name}_not_beyond_undelivered"] = {"ok": not passed, "cursor": list(c), "undelivered_behind": sorted(passed)[:20]}
    return out


# ── scenario plumbing ───────────────────────────────────────────────────────

class Scenario:
    def __init__(self, name: str, flash_bytes: int = PARTITION_BYTES):
        self.name = name
        self.root = runs_root() / name
        if self.root.exists():
            shutil.rmtree(self.root)
        self.root.mkdir(parents=True)
        self.env = {"EVQ_FLASH_BYTES": str(flash_bytes)}
        self.state = self.root / "state"
        self.state.mkdir()
        (self.state / "out").mkdir()
        self.steps: list[dict] = []
        self.drops: list[dict] = []
        self.home: dict[int, int] = {}     # id -> flash seq it was first seen in
        self.result: dict = {"scenario": name, "steps": self.steps, "checks": {}, "drops": self.drops}

    def step(self, fw: str, label: str, script: list[str]) -> dict:
        before = snapshot(self.state)
        loc0 = locate(self.state)
        r = run(fw, self.state, script, self.env)
        after = snapshot(self.state)
        self._track_drops(fw, label, loc0, locate(self.state), r["stderr_all"])
        st = {"fw": fw, "label": label, "script": script, "diff": diff(before, after),
              "health_boot": _slim(r["health"].get("boot", {})),
              "health_end": _slim(r["health"].get("pre-shutdown") or r["health"].get("pre-crash") or {}),
              "warnings": r["stderr_warnings"]}
        self.steps.append(st)
        return st

    def _track_drops(self, fw: str, label: str, loc0: dict, loc1: dict, log: str) -> None:
        for i, xs in loc1.items():
            for x in xs:
                if x["seq"] is not None:
                    self.home.setdefault(i, x["seq"])
        for i, xs in loc0.items():
            for x in xs:
                if x["seq"] is not None:
                    self.home.setdefault(i, x["seq"])
        gone = sorted(i for i in loc0 if i not in loc1)
        if not gone:
            return
        evicted = {int(m) for m in re.findall(r"evicted (?:delivered|synced) \S*ev-(\d+)\.log", log)}
        reimported = {int(m) for m in re.findall(r"re-imported ev-(\d+)\.log", log)}
        seqs = sorted({self.home.get(i) for i in gone} - {None})
        why = {q: ("eviction of a delivered flash copy (store short of space)" if q in evicted else
                   "re-import retired the flash source (no archive copy of its delivered prefix)" if q in reimported
                   else "unattributed") for q in seqs}
        # v1.11.0's own removals are the baseline's behaviour on this device
        # already; only the candidate is held to the retention invariant.
        unexplained = [] if fw == "v1.11.0" else [
            i for i in gone if not why.get(self.home.get(i), "unattributed").startswith("eviction")]
        self.drops.append({"fw": fw, "step": label, "n": len(gone), "ids": _ranges(gone),
                           "flash_seqs": {str(q): why[q] for q in seqs}, "unexplained_ids": _ranges(unexplained)})

    def save(self, label: str) -> Path:
        dst = self.root / f"snap-{len(self.steps):02d}-{label}"
        shutil.copytree(self.state, dst, symlinks=True)
        return dst

    def views(self, label: str, strict: bool, corpus: dict | None = None, must: set | None = None,
              probes: tuple[str, ...] = ("v1.11.0", "candidate"), pre: dict | None = None,
              sd_root: Path | None = None, rounds: int = 0) -> dict:
        corpus = corpus if corpus is not None else corpus_from_logs(self.state)
        must = must if must is not None else set(corpus) - acked_ids(self.state)
        out = {"corpus": corpus_check(self.state, corpus, must, sd_root, list(self.drops)),
               "cursor": cursor_checks(self.state, must),
               "static": {}, "probe": {}}
        vs, cs = v1110_static(self.state), cand_static(self.state)
        out["static"] = {"v1.11.0": _sv(vs), "candidate": _sv(cs),
                         "views_agree": {"ok": [i for i, _ in vs["pending"]] == [i for i, _ in cs["pending"]]}}
        for fw in probes:
            pr = probe(fw, self.state, (pre or {}).get(fw), self.env, tag=f"-{label}", rounds=rounds)
            chk = evaluate(self.state, corpus, must, pr, strict)
            st = vs if fw == "v1.11.0" else cs
            chk["static_matches_probe"] = {"ok": [i for i, _ in st["pending"]] == [i for i, _ in pr["order"]]
                                           or bool((pre or {}).get(fw)) or rounds > 0, "static_n": len(st["pending"]),
                                           "probe_n": len(pr["order"])}
            out["probe"][fw] = {"checks": chk, "end": _slim(pr["end"]), "start": _slim(pr["start"])}
        self.result["checks"][label] = out
        return out


def _sv(s: dict) -> dict:
    return {"cursor": s["cursor"], "pending": len(s["pending"]),
            "first_id": s["pending"][0][0] if s["pending"] else None,
            "last_id": s["pending"][-1][0] if s["pending"] else None, "gaps": s["gaps"], "bad": s["bad"][:5],
            **({"foreign": s["foreign"], "sd_used": s["sd_used"][:5]} if "foreign" in s else {})}


def _slim(h: dict) -> dict:
    keep = ("pending", "next_id", "rd_seq", "rd_off", "tail_seq", "skipped", "dropped", "skipped_unindexed_gap",
            "quarantined_malformed", "corrupt_detected", "sd_state", "head_block", "index_segments", "spool_files",
            "reclaimed", "archived", "reimported", "reimport_pending", "sd_pending")
    return {k: h[k] for k in keep if k in h}


def failures(res: dict) -> list[str]:
    """Every failed check, as 'label/where/check'."""
    bad = []
    for label, v in res["checks"].items():
        if not v["corpus"]["ok"]:
            bad.append(f"{label}/corpus/undelivered_intact")
        if not v["corpus"]["delivered_ok"]:
            bad.append(f"{label}/corpus/delivered_retained")
        for k, c in v["cursor"].items():
            if isinstance(c, dict) and "ok" in c and not c["ok"]:
                bad.append(f"{label}/cursor/{k}")
        if not v["static"]["views_agree"]["ok"]:
            bad.append(f"{label}/static/views_agree")
        for fw, p in v["probe"].items():
            bad += [f"{label}/{fw}/{k}" for k, c in p["checks"].items() if not c["ok"]]
    return bad


# ── synthetic scenarios ─────────────────────────────────────────────────────

BENCH_FIRST_ID = 387242 - 309      # bench: next_id 387242 after 309 pending records


def seed_v1110(sc: Scenario, profile: str, acked: int) -> None:
    nvs_write(sc.state, {"evlog": {"nid": {"type": "u64", "value": BENCH_FIRST_ID}}})
    script = [f"store 309 {profile}"] + ([f"deliver {acked} acked-v1110-seed.jsonl"] if acked else [])
    sc.step("v1.11.0", "v1.11.0 creates the store", script)


def bench_shape(sc: Scenario) -> dict:
    ff = flash_files(sc.state)
    cur = cursor_nvs(nvs_read(sc.state))
    h = health(sc.state, "pre-shutdown")
    return {"files": [min(ff), max(ff)], "cursor_legacy": cur["legacy"], "next_id": h.get("next_id"),
            "nid_hwm": cur["nid"], "pending": len(corpus_from_logs(sc.state)) - len(acked_ids(sc.state)),
            "evstore_bytes": sum(p.stat().st_size for p in ff.values())}


def first_boot(sc: Scenario, sd: bool) -> None:
    """The candidate's first boot on the v1.11.0 state: open (+ migration), claim
    without ack, keeper passes, graceful reboot (the health gate's rollback)."""
    sc.save("v1110-state")
    script = (["sd insert"] if sd else []) + ["peek 16 peek-cand-first.jsonl", "service 3", "health after-keeper"]
    st = sc.step("candidate", "candidate first boot", script)
    sc.result["first_boot_changes"] = st["diff"]
    sc.result["first_boot_nvs_ops"] = nvs_ops(sc.state, "candidate")


def nvs_ops(state: Path, fw: str) -> dict:
    ops = [o for o in jsonl(state / ".shim" / "nvs_ops.jsonl") if o["fw"] == fw]
    w = sorted({f"{o['ns']}/{o['key']}:{o['type']}" for o in ops if o["op"] in ("set", "erase_key", "erase_all") and o["rc"] == 0})
    r = sorted({f"{o['ns']}/{o['key']}:{o['type']}" for o in ops if o["op"] == "get"})
    nss = sorted({o["ns"] for o in ops if o["op"].startswith("open")})
    return {"namespaces_opened": nss, "keys_read": r, "keys_written": w,
            "type_conflicts": sum(1 for o in ops if o["op"] == "type_conflict")}


def sc_bench_cursor0() -> dict:
    """The bench device's exact state: 309 pending in files 1..1, cursor 1:0,
    next_id 387242. Candidate first boot, then v1.11.0 reopen."""
    sc = Scenario("bench_cursor0")
    seed_v1110(sc, "bench", 0)
    sc.result["v1110_shape"] = bench_shape(sc)
    sc.views("v1110-created", strict=True)
    first_boot(sc, sd=False)
    sc.views("after-cand-first-boot", strict=True)
    st = sc.step("v1.11.0", "v1.11.0 reopen (rollback, no-op boot)", ["health reopened"])
    sc.result["v1110_reopen_changes"] = st["diff"]
    sc.views("after-v1110-reopen", strict=True)
    return sc.result


def v1110_era_card(sd: Path) -> None:
    """A card as a v1.11.0 bench unit leaves it: a bulk archive of delivered
    records, sd_logger output, an AMBIT image. None of it is queue data."""
    (sd / "archive").mkdir(parents=True)
    lines = [f"{i}\tuart_0\t28:37:2F:FF:E7:04\tMEASUREMENT\tarrun x\t{1757000000000 + i}\t{1757000001000 + i}\t\t"
             f"{{\"archived\":true,\"k\":{i}}}\n" for i in range(100000, 100200)]
    (sd / "archive" / "arc-100000.log").write_text("".join(lines))
    (sd / "log").mkdir()
    (sd / "log" / "sd-000001.txt").write_text("sd_logger line\n" * 50)
    (sd / "ambit_fw.bin").write_bytes(hashlib.sha256(b"fw").digest() * 512)


def sc_bench_cursor0_sd() -> dict:
    """bench_cursor0 with the bench unit's SD card in the slot (v1.11.0-era
    contents): the candidate's first-boot keeper sees a card."""
    sc = Scenario("bench_cursor0_sd")
    seed_v1110(sc, "bench", 0)
    v1110_era_card(sc.state / "sdcard")
    sc.result["v1110_shape"] = bench_shape(sc)
    sd0 = {k: v for k, v in snapshot(sc.state)["files"].items() if k.startswith("sdcard/")}
    first_boot(sc, sd=True)
    sc.views("after-cand-first-boot", strict=True)
    st = sc.step("v1.11.0", "v1.11.0 reopen with the card", ["service 2", "health reopened"])
    sc.result["v1110_reopen_changes"] = st["diff"]
    sc.views("after-v1110-reopen", strict=True, pre={"v1.11.0": ["service 3"]})
    sd1 = {k: v for k, v in snapshot(sc.state)["files"].items() if k.startswith("sdcard/")}
    sc.result["preexisting_sd_files_changed"] = sorted(k for k in sd0 if sd1.get(k) != sd0[k])
    return sc.result


def sc_bench_cursor_gt0() -> dict:
    """309 records (multi-KB, several rotated files) with a delivered prefix:
    cursor > 0 mid-file. Candidate first boot, then v1.11.0 reopen."""
    sc = Scenario("bench_cursor_gt0")
    seed_v1110(sc, "mixed", 100)
    sc.result["v1110_shape"] = bench_shape(sc)
    sc.views("v1110-created", strict=True)
    first_boot(sc, sd=False)
    sc.views("after-cand-first-boot", strict=True)
    st = sc.step("v1.11.0", "v1.11.0 reopen (rollback, no-op boot)", ["health reopened"])
    sc.result["v1110_reopen_changes"] = st["diff"]
    sc.views("after-v1110-reopen", strict=True)
    return sc.result


def sc_cand_acks_stores_rotates() -> dict:
    """Candidate acks a prefix across file boundaries (D lines, cursor blob),
    stores 400 more (rotations -> S lines), keeper passes; then v1.11.0."""
    sc = Scenario("cand_acks_stores_rotates")
    seed_v1110(sc, "mixed", 100)
    first_boot(sc, sd=False)
    st = sc.step("candidate", "candidate session 2", ["deliver 150 acked-cand-2.jsonl", "store 400 mixed",
                                                        "service 3", "health s2"])
    sc.result["cand_session_changes"] = st["diff"]
    sc.views("after-cand-session", strict=True)
    st = sc.step("v1.11.0", "v1.11.0 reopen", ["health reopened"])
    sc.result["v1110_reopen_changes"] = st["diff"]
    sc.views("after-v1110-reopen", strict=True)
    return sc.result


def sc_cand_crash() -> dict:
    """Candidate acks 37 (not a multiple of the 16-ack cursor batch), stores 50
    and dies WITHOUT the shutdown handler (panic/WDT/brownout-reset): the
    persisted cursor lags, so up to 15 acked records may replay (duplicates,
    never a skip); then v1.11.0."""
    sc = Scenario("cand_crash")
    seed_v1110(sc, "mixed", 100)
    first_boot(sc, sd=False)
    sc.step("candidate", "candidate session 2 (crash)", ["deliver 37 acked-cand-2.jsonl", "store 50 mixed", "crash"])
    sc.views("after-cand-crash", strict=False)
    sc.step("v1.11.0", "v1.11.0 reopen", ["health reopened"])
    sc.views("after-v1110-reopen", strict=False)
    return sc.result


def sc_sd_present() -> dict:
    """SD card present: the candidate's keeper runs (card repair/legacy scan),
    a 1000-store burst makes it spool unsent rotated files to
    /sdcard/events + /sdcard/evq and archive delivered ones. v1.11.0 then
    reopens with the card (its keeper imports the SD primaries) and without."""
    sc = Scenario("sd_present")
    seed_v1110(sc, "mixed", 100)
    (sc.state / "sdcard").mkdir()
    first_boot(sc, sd=True)
    sc.views("after-cand-first-boot", strict=True)
    st = sc.step("candidate", "candidate session 2 (1100 stores, burst)",
                 ["store 1100 small", "deliver 200 acked-cand-2.jsonl", "service 3", "health s2"])
    sc.result["cand_session_changes"] = {k: (v if k == "nvs" else len(v)) for k, v in st["diff"].items()}
    sc.result["cand_session_changes_sd"] = [f for f in st["diff"]["created"] if f.startswith("sdcard/")][:40]
    # v1.11.0 with the card: its keeper (import + archive) runs before delivery
    sc.views("v1110-with-card", strict=False, probes=("v1.11.0",), pre={"v1.11.0": ["service 3"]})
    # v1.11.0 without the card (card pulled at rollback)
    save = sc.state / "sdcard"
    hidden = sc.root / "sdcard-hidden"
    save.rename(hidden)
    (sc.state / ".shim" / "sd_state").unlink(missing_ok=True)
    try:
        sc.views("v1110-no-card", strict=True, probes=("v1.11.0",), sd_root=hidden)
    finally:
        hidden.rename(save)
    return sc.result


def sc_reupgrade() -> dict:
    """Round trip candidate -> v1.11.0 (which acks and stores) -> candidate: the
    legacy keys are now AHEAD of the stale `cur` blob (foreign cursor move)."""
    sc = Scenario("reupgrade")
    seed_v1110(sc, "mixed", 100)
    first_boot(sc, sd=False)
    sc.step("candidate", "candidate session 2", ["deliver 150 acked-cand-2.jsonl", "store 400 mixed", "service 3"])
    sc.step("v1.11.0", "v1.11.0 session", ["deliver 60 acked-v1110-2.jsonl", "store 30 mixed"])
    sc.views("after-v1110-session", strict=True)
    st = sc.step("candidate", "candidate re-boot", ["service 3", "health again"])
    sc.result["reupgrade_changes"] = st["diff"]
    sc.views("after-cand-reboot", strict=False, probes=("candidate", "v1.11.0"))
    return sc.result


def sc_pressure() -> dict:
    """OUTSIDE the bench envelope (flash pressure): 3 MiB partition, card
    present. The candidate spools unsent files and RECLAIMS their flash copies
    (SD_ONLY). v1.11.0 knows no index: this documents what it then does."""
    sc = Scenario("pressure", flash_bytes=3 * 1024 * 1024)
    seed_v1110(sc, "mixed", 100)
    (sc.state / "sdcard").mkdir()
    first_boot(sc, sd=True)
    st = sc.step("candidate", "candidate fills flash", ["store 350 mixed", "service 3", "health s2"])
    sc.result["cand_session_changes_n"] = {k: (v if k == "nvs" else len(v)) for k, v in st["diff"].items()}
    # rounds: v1.11.0's import stops below 256 KiB free and resumes as delivery frees flash
    sc.views("v1110-with-card", strict=False, probes=("v1.11.0",), pre={"v1.11.0": ["service 3"]}, rounds=12)
    hidden = sc.root / "sdcard-hidden"
    (sc.state / "sdcard").rename(hidden)
    (sc.state / ".shim" / "sd_state").unlink(missing_ok=True)
    try:
        sc.views("v1110-no-card", strict=True, probes=("v1.11.0",), sd_root=hidden)
    finally:
        hidden.rename(sc.state / "sdcard")
    return sc.result


SCENARIOS = {
    "bench_cursor0": sc_bench_cursor0,
    "bench_cursor0_sd": sc_bench_cursor0_sd,
    "bench_cursor_gt0": sc_bench_cursor_gt0,
    "cand_acks_stores_rotates": sc_cand_acks_stores_rotates,
    "cand_crash": sc_cand_crash,
    "sd_present": sc_sd_present,
    "reupgrade": sc_reupgrade,
    "pressure": sc_pressure,
}


# ── negative controls (the oracle must catch a broken migration) ───────────

def negative_control(kind: str, src: Path) -> dict:
    """Tamper with a copy of a v1.11.0-created state the way a broken
    migration would, then run the full check. Every kind MUST fail."""
    sc = Scenario(f"neg_{kind}")
    shutil.rmtree(sc.state)
    shutil.copytree(src, sc.state, symlinks=True)
    ff = flash_files(sc.state)
    first = next(r for r in parse_lines(ff[min(ff)]))
    if kind == "cursor_skip":        # legacy cursor moved past one undelivered record
        nv = nvs_read(sc.state)
        nv["evlog"]["rd_off"] = {"type": "u32", "value": first["off"] + first["len"]}
        nvs_write(sc.state, nv)
    elif kind == "line_corrupt":     # one byte of a pending record changed in place
        b = bytearray(ff[min(ff)].read_bytes())
        b[first["len"] - 3] ^= 0x01
        ff[min(ff)].write_bytes(bytes(b))
    elif kind == "file_gone":        # a queue file deleted
        ff[min(ff)].unlink()
    sc.views("tampered", strict=True)
    sc.result["failures"] = failures(sc.result)
    return sc.result


# ── real device dump ────────────────────────────────────────────────────────

def check_dump(evstore: Path, nvs_json: Path, sdcard: Path | None, name: str = "device_dump") -> dict:
    """Round trip on a copy of a real device's /evstore + NVS: the corpus and
    must_have come from the dump itself (records at/after its v1.11.0 cursor)."""
    sc = Scenario(name)
    shutil.copytree(evstore, sc.state / "evstore")
    if sdcard:
        shutil.copytree(sdcard, sc.state / "sdcard")
    nvs_write(sc.state, json.loads(Path(nvs_json).read_text()))
    loc = locate(sc.state)
    corpus = {}
    for i, xs in loc.items():
        shas = {x["sha"] for x in xs if x["role"] == "flash"}
        if len(shas) > 1:
            raise AssertionError(f"dump holds two different lines for id {i}")
        if shas:
            corpus[i] = shas.pop()
    vs = v1110_static(sc.state)
    must = {i for i, _ in vs["pending"]}
    sc.result["dump"] = {"records": len(corpus), "must_have": len(must), "v1110_cursor": vs["cursor"],
                         "nvs": cursor_nvs(nvs_read(sc.state))}
    sc.views("dump", strict=True, corpus=corpus, must=must)
    first_boot(sc, sd=sdcard is not None)
    sc.views("after-cand-first-boot", strict=True, corpus=corpus, must=must)
    st = sc.step("v1.11.0", "v1.11.0 reopen", ["health reopened"])
    sc.result["v1110_reopen_changes"] = st["diff"]
    sc.views("after-v1110-reopen", strict=True, corpus=corpus, must=must)
    return sc.result


def export_dump(state: Path, dst: Path) -> tuple[Path, Path, Path | None]:
    """Write a state in the device-dump input format (evstore/, nvs.json, sdcard/)."""
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    shutil.copytree(state / "evstore", dst / "evstore")
    nv = {ns: {k: {"type": e["type"], "hex": e["hex"]} for k, e in keys.items()} for ns, keys in nvs_read(state).items()}
    (dst / "nvs.json").write_text(json.dumps(nv, indent=1))
    sd = None
    if (state / "sdcard").is_dir():
        shutil.copytree(state / "sdcard", dst / "sdcard")
        sd = dst / "sdcard"
    return dst / "evstore", dst / "nvs.json", sd


def summary(r: dict) -> dict:
    """One compact JSON line per scenario (the full result is saved alongside)."""
    out = {"scenario": r["scenario"], "failures": r.get("failures", failures(r)),
           "v1110_shape": r.get("v1110_shape"), "drops": r["drops"], "views": {}}
    for label, v in r["checks"].items():
        out["views"][label] = {
            "corpus": {k: v["corpus"][k] for k in ("records", "undelivered_missing", "delivered_gone",
                                                   "not_on_flash_by_location")} | {"index": v["corpus"]["index"]["states"]},
            "cursor_nvs": v["cursor"]["nvs"],
            "static": {fw: v["static"][fw]["cursor"] + [v["static"][fw]["pending"]] for fw in ("v1.11.0", "candidate")},
            "probe": {fw: {"delivered": p["checks"]["exact_pending"]["delivered"],
                           "must_have": p["checks"]["exact_pending"]["must_have"],
                           "redelivered_acked": p["checks"]["exact_pending"]["redelivered_acked"],
                           "duplicates": p["checks"]["exact_pending"]["duplicates_in_probe"],
                           "failed": [k for k, c in p["checks"].items() if not c["ok"]]}
                      for fw, p in v["probe"].items()}}
    if "first_boot_changes" in r:
        fb = r["first_boot_changes"]
        out["candidate_first_boot"] = {"files_created": fb["created"], "files_removed": fb["removed"],
                                       "files_modified": fb["modified"],
                                       "nvs": [f"{n['ns']}/{n.get('key', '')}" for n in fb["nvs"]],
                                       "nvs_ops": r.get("first_boot_nvs_ops")}
    if "v1110_reopen_changes" in r:
        rc = r["v1110_reopen_changes"]
        out["v1110_reopen"] = {k: rc[k] for k in ("created", "removed", "modified")} | {"nvs": len(rc["nvs"])}
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scenarios")
    s.add_argument("names", nargs="*")
    c = sub.add_parser("check")
    c.add_argument("--evstore", required=True, type=Path)
    c.add_argument("--nvs", required=True, type=Path)
    c.add_argument("--sdcard", type=Path)
    a = ap.parse_args()
    results = []
    if a.cmd == "scenarios":
        for n in a.names or list(SCENARIOS):
            results.append(SCENARIOS[n]())
    else:
        results.append(check_dump(a.evstore, a.nvs, a.sdcard))
    rc = 0
    res_dir = B.tmp_root() / "results"
    res_dir.mkdir(exist_ok=True)
    for r in results:
        r["failures"] = failures(r)
        rc |= bool(r["failures"])
        (res_dir / f"{r['scenario']}.json").write_text(json.dumps(r, indent=1, default=str))
        print(json.dumps(summary(r), default=str))
    return rc


if __name__ == "__main__":
    sys.exit(main())

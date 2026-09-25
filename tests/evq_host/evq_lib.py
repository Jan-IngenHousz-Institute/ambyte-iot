"""Storage harness library: device runner, power-loss transform, independent
oracles and evidence writer (tests/evq_host).

Nothing here imports or re-implements production logic beyond the DOCUMENTED
on-disk formats (v2 record lines, the evq.idx line format, the NVS cursor
keys): the oracles read raw files and decide independently whether every
accepted record is still recoverable and was finally delivered intact.
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import tempfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path

HOST = Path(__file__).resolve().parent
ROOT = HOST.parents[1]


class HarnessError(RuntimeError):
    pass


class OracleFailure(AssertionError):
    pass


# ── fault-point registry (derived from the production sources) ─────────────
FP_LITERAL = re.compile(r'EVQ_FAULT_POINT\("([^"]+)"\)')
FP_COMPOSED = re.compile(r'snprintf\(name, sizeof name, "%s\.([a-z_.]+)", fp\)')
FP_PREFIXES = ["spool", "mirror", "archive"]


def registered_fault_points() -> list[str]:
    names: set[str] = set()
    for src in ["components/event_log/event_log.c", "components/event_log/evq_index.c"]:
        text = (ROOT / src).read_text()
        names.update(FP_LITERAL.findall(text))
        suffixes = FP_COMPOSED.findall(text)
        for p in FP_PREFIXES:
            for s in suffixes:
                names.add(f"{p}.{s}")
    names.discard("name")
    return sorted(names)


# ── helpers ────────────────────────────────────────────────────────────────
def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def media_hashes(state: Path) -> dict[str, str]:
    out = {}
    for base in ["evstore", "sdcard", "cards"]:
        d = state / base
        if not d.exists():
            continue
        for p in sorted(d.rglob("*")):
            if p.is_file():
                out[str(p.relative_to(state))] = sha256_file(p)
    return out


def read_jsonl(p: Path) -> list[dict]:
    if not p.exists():
        return []
    out = []
    for line in p.read_text().splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


def nvs_read(state: Path) -> dict[tuple[str, str], bytes]:
    out = {}
    p = state / "nvs.txt"
    if p.exists():
        for line in p.read_text().split("\n"):
            parts = line.split()
            if len(parts) == 3:
                out[(parts[0], parts[1])] = bytes.fromhex(parts[2])
    return out


def nvs_write(state: Path, kv: dict[tuple[str, str], bytes]) -> None:
    lines = [f"{ns} {k} {v.hex()}" for (ns, k), v in kv.items()]
    (state / "nvs.txt").write_text("\n".join(lines) + "\n")


def cursor_from_nvs(state: Path) -> tuple[int, int, dict]:
    """Independent reading of the documented cursor keys: the atomic blob
    `evlog/cur` {seq, off, crc} and the legacy `rd_seq`/`rd_off`. Returns the
    CONSERVATIVE (earliest) of the two."""
    kv = nvs_read(state)
    info = {}
    legacy = None
    if ("evlog", "rd_seq") in kv:
        seq = int.from_bytes(kv[("evlog", "rd_seq")], "little")
        off = int.from_bytes(kv.get(("evlog", "rd_off"), b"\0\0\0\0"), "little")
        legacy = (seq, off)
        info["legacy"] = legacy
    blob = None
    if ("evlog", "cur") in kv and len(kv[("evlog", "cur")]) == 12:
        b = kv[("evlog", "cur")]
        blob = (int.from_bytes(b[0:4], "little"), int.from_bytes(b[4:8], "little"))
        info["blob"] = blob
    cands = [c for c in (legacy, blob) if c is not None]
    if not cands:
        return 0, 0, info
    seq, off = min(cands)
    return seq, off, info


# ── record parsing ─────────────────────────────────────────────────────────
def iter_records(data: bytes):
    """Yield (offset, line_bytes) for complete, newline-terminated lines."""
    pos = 0
    n = len(data)
    while pos < n:
        nl = data.find(b"\n", pos)
        if nl < 0:
            return
        yield pos, data[pos:nl + 1]
        pos = nl + 1


def record_id(line: bytes) -> int | None:
    m = re.match(rb"(\d+)\t", line)
    return int(m.group(1)) if m else None


def line_sha(line: bytes) -> str:
    return hashlib.sha256(line).hexdigest()


# ── segment index (documented format) ──────────────────────────────────────
@dataclass
class Seg:
    seq: int
    first: int = 0
    last: int = 0
    count: int = 0
    bytes: int = 0
    crc: int = 0
    state: str = "FLASH"
    cid: int = 0
    primary: str = ""
    mirror: str = ""
    reimport_off: int = 0


def parse_index(data: bytes) -> tuple[dict[int, Seg], int, int | None]:
    segs: dict[int, Seg] = {}
    bad = 0
    wm = None
    for raw in data.split(b"\n"):
        if not raw:
            continue
        line = raw.decode("latin-1")
        m = re.match(r"^(.*) \*([0-9a-f]{8})$", line)
        if not m or (zlib.crc32(m.group(1).encode("latin-1")) & 0xFFFFFFFF) != int(m.group(2), 16):
            bad += 1
            continue
        f = m.group(1).split(" ")
        k = f[0]
        try:
            if k == "S":
                s = Seg(int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5]), int(f[6], 16))
                segs[s.seq] = s
            elif k == "P" and int(f[1]) in segs:
                s = segs[int(f[1])]
                s.state, s.cid, s.primary, s.mirror = "SPOOLED", int(f[2], 16), f[3], f[4]
            elif k in "ODA" and int(f[1]) in segs:
                if k == "A":
                    del segs[int(f[1])]
                else:
                    segs[int(f[1])].state = "SD_ONLY" if k == "O" else "DELIVERED"
            elif k == "R" and int(f[1]) in segs:
                segs[int(f[1])].state = "REIMPORT"
                segs[int(f[1])].reimport_off = int(f[2])
            elif k == "W":
                wm = (int(f[1]), int(f[2]))
            else:
                bad += 1
        except (ValueError, IndexError):
            bad += 1
    return segs, bad, wm


# ── device runner ──────────────────────────────────────────────────────────
@dataclass
class Device:
    state: Path
    exe: Path
    seed: int = 1
    scenario: str = "x"
    env: dict = field(default_factory=dict)
    boots: int = 0
    crashes: int = 0
    graceful: int = 0
    checkpoints: int = 0
    transforms: list = field(default_factory=list)
    cp_hook: object = None      # callable(device, label) -> None (raises OracleFailure)
    scripts: int = 0
    durability_mode: str = "assert"   # "record": baseline runs log R-DUR findings instead of failing
    durability_findings: list = field(default_factory=list)
    allow_lost: set = field(default_factory=set)   # G8c only: ids whose bytes the test itself corrupted

    def __post_init__(self):
        self.state.mkdir(parents=True, exist_ok=True)
        (self.state / "evstore").mkdir(exist_ok=True)
        (self.state / "sdcard").mkdir(exist_ok=True)
        (self.state / ".shim").mkdir(exist_ok=True)
        self.rng = random.Random(self.seed * 104729 + len(self.scenario))

    armed: str | None = None     # fault spec kept armed across phases until it fires

    def fault_fired(self) -> bool:
        p = self.state / ".shim" / "fault_fired"
        return p.exists() and self.armed is not None and self.armed.split(":")[0] in p.read_text()

    def run(self, lines: list[str], fault: str | None = None, exe: Path | None = None,
            max_phases: int = 200) -> dict:
        exe = exe or self.exe
        if fault and fault != self.armed:
            self.armed = fault
            (self.state / ".shim" / "fault_hits").unlink(missing_ok=True)
        self.scripts += 1
        script = self.state / ".shim" / f"script{self.scripts}.txt"
        script.write_text("\n".join(lines) + "\n")
        start = 1
        for _ in range(max_phases):
            fault_env = self.armed if (self.armed and not self.fault_fired()) else None
            env = dict(os.environ)
            env.update({"EVQ_SEED": str(self.seed), "EVQ_SCENARIO": self.scenario, "EVQ_BOOT": str(self.boots),
                        "EVQ_QUIET": "1", "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
            env.update(self.env)
            env.pop("EVQ_FAULT", None)
            if fault_env:
                env["EVQ_FAULT"] = fault_env
            errp = self.state / ".shim" / f"stderr.{self.boots}"
            with open(errp, "w") as errf:
                p = subprocess.Popen([str(exe), str(script), str(start)], cwd=self.state, env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errf, text=True)
                assert p.stdout is not None and p.stdin is not None
                for out in p.stdout:
                    out = out.strip()
                    if out.startswith("CHECKPOINT "):
                        label = out.split(" ", 1)[1]
                        self.checkpoints += 1
                        try:
                            if self.cp_hook:
                                self.cp_hook(self, label)
                            if self.durability_mode == "record":
                                try:
                                    check_durability(self)
                                except OracleFailure as e:
                                    self.durability_findings.append({"checkpoint": label, "finding": str(e)})
                            else:
                                check_durability(self)
                        except OracleFailure as e:
                            p.stdin.write("FAIL\n"); p.stdin.flush()
                            p.wait()
                            raise OracleFailure(f"checkpoint {label}: {e}") from None
                        p.stdin.write("OK\n")
                        p.stdin.flush()
                rc = p.wait()
            self.boots += 1
            err = errp.read_text()
            if "ERROR: AddressSanitizer" in err or "runtime error:" in err or "LeakSanitizer" in err or "FATAL:" in err:
                raise HarnessError(f"sanitizer/fatal report (boot {self.boots - 1}):\n{err[-4000:]}")
            if rc == 0:
                (self.state / ".shim" / "journal").unlink(missing_ok=True)
                return {"boots": self.boots, "crashes": self.crashes}
            pcf = self.state / "out" / "pc"
            pc = int(pcf.read_text().strip() or "0") if pcf.exists() else start - 1
            if rc == 85:
                self.graceful += 1
                (self.state / ".shim" / "journal").unlink(missing_ok=True)
                start = pc + 1
            elif rc == 86:
                self.crashes += 1
                self.transforms.append(power_transform(self))
                # a crashed recovery command (drain_all) is re-run after the reboot;
                # any other command is abandoned, as a real interrupted operation would be
                crashed_cmd = lines[pc - 1].split(" ")[0] if 0 < pc <= len(lines) else ""
                start = pc if crashed_cmd == "drain_all" else pc + 1
            else:
                raise HarnessError(f"driver exited {rc} at line {pc}:\n{err[-4000:]}")
            if start > len(lines):
                # crashed/rebooted on the final command: boot once more so recovery runs
                lines = lines + ["health reboot-tail"]
                script.write_text("\n".join(lines) + "\n")
        raise HarnessError("too many phases")


# ── power-loss transform ───────────────────────────────────────────────────
def _journal(state: Path) -> list[list[str]]:
    p = state / ".shim" / "journal"
    if not p.exists():
        return []
    return [l.split(" ") for l in p.read_text().splitlines() if l.strip()]


def durable_view(state: Path) -> dict[str, dict]:
    """path → {'medium': 1|2, 'durable': size, 'new': bool} for files written
    since the last boot (files not listed are fully durable)."""
    view: dict[str, dict] = {}
    for f in _journal(state):
        k = f[0]
        if k == "O":
            path = f[2]
            if path not in view:
                view[path] = {"medium": int(f[1]), "durable": int(f[3]), "new": f[4] == "1"}
        elif k == "D":
            path = f[2]
            e = view.setdefault(path, {"medium": int(f[1]), "durable": 0, "new": False})
            e["durable"] = int(f[3])
            e["new"] = False
        elif k in ("R", "L"):
            a, b = f[2], f[3]
            if a in view:
                view[b] = dict(view[a])
                if k == "R":
                    del view[a]
        elif k == "X":
            view.pop(f[2], None)
    return view


def power_transform(dev: Device) -> dict:
    """Simulated power loss. Flash (littlefs, copy-on-write): every file
    reverts to its last synced size. SD (FAT, adversarial): a file written
    since its last sync is cut to a seeded length between its synced and
    current size, sometimes with garbage appended; a never-synced new file may
    vanish. Directory operations that RETURNED are durable (ESP-IDF FatFs
    f_rename/f_unlink/f_mkdir end in sync_fs; littlefs is atomic); a crash
    INSIDE one is resolved by the shim itself before it exits."""
    st = dev.state
    rng = dev.rng
    applied = []
    for path, e in durable_view(st).items():
        p = st / path
        if not p.exists():
            continue
        cur = p.stat().st_size
        if e["medium"] == 1:
            if cur > e["durable"]:
                with open(p, "r+b") as f:
                    f.truncate(e["durable"])
                applied.append({"path": path, "from": cur, "to": e["durable"], "medium": "flash"})
        else:
            if e["new"] and rng.random() < 0.5:
                p.unlink()
                applied.append({"path": path, "vanished": True, "medium": "sd"})
                continue
            if cur > e["durable"]:
                cut = rng.randint(e["durable"], cur)
                with open(p, "r+b") as f:
                    f.truncate(cut)
                    if rng.random() < 0.3:
                        f.seek(cut)
                        f.write(bytes(rng.randrange(256) for _ in range(rng.randint(1, 48))))
                applied.append({"path": path, "from": cur, "to": cut, "medium": "sd"})
    (st / ".shim" / "journal").unlink(missing_ok=True)
    return {"boot": dev.boots, "changes": applied}


# ── oracles ────────────────────────────────────────────────────────────────
def _card_dirs(state: Path) -> list[Path]:
    dirs = [state / "sdcard"] if (state / "sdcard").exists() else []
    if (state / "cards").exists():
        dirs += [d for d in sorted((state / "cards").iterdir()) if d.is_dir()]
    return dirs


def _durable_bytes(state: Path, rel: str, view: dict) -> bytes:
    data = (state / rel).read_bytes()
    e = view.get(rel)
    if e is not None and len(data) > e["durable"]:
        data = data[: e["durable"]]
    return data


def reachable_copies(state: Path) -> dict[int, set[str]]:
    """id → set of line sha256 found in locations the documented states make
    reachable by the queue, using only DURABLE bytes (what would survive a
    power loss right now)."""
    view = durable_view(state)
    cseq, coff, _ = cursor_from_nvs(state)
    idx_rel = "evstore/evq.idx"
    segs, _, _ = parse_index(_durable_bytes(state, idx_rel, view) if (state / idx_rel).exists() else b"")
    found: dict[int, set[str]] = {}

    def add(data: bytes, min_off: int = 0):
        for off, line in iter_records(data):
            if off < min_off:
                continue
            rid = record_id(line)
            if rid is not None:
                found.setdefault(rid, set()).add(line_sha(line))

    ev = state / "evstore" / "events"
    if ev.exists():
        for p in ev.glob("ev-*.log"):
            m = re.match(r"ev-(\d+)\.log$", p.name)
            if not m:
                continue
            seq = int(m.group(1))
            if seq < cseq:
                # cursor passed it: reachable only as an indexed REIMPORT source
                s = segs.get(seq)
                if s is not None and s.state == "REIMPORT":
                    add(_durable_bytes(state, str(p.relative_to(state)), view), s.reimport_off)
                continue
            add(_durable_bytes(state, str(p.relative_to(state)), view), coff if seq == cseq else 0)
    named = {}
    for s in segs.values():
        named[("events", s.primary)] = s
        named[("evq", s.mirror)] = s
    for card in _card_dirs(state):
        for sub in ["events", "evq"]:
            d = card / sub
            if not d.exists():
                continue
            for p in d.glob("*.log"):
                rel = str(p.relative_to(state))
                s = named.get((sub, p.name))
                data = _durable_bytes(state, rel, view)
                if s is not None:
                    if s.state in ("DELIVERED",) and s.seq < cseq:
                        continue
                    if s.state == "REIMPORT":
                        add(data, s.reimport_off)
                    elif s.seq < cseq:
                        continue       # passed by our own cursor: delivered (archive pending)
                    else:
                        add(data, coff if s.seq == cseq else 0)
                else:
                    add(data)          # unindexed: legacy-import / orphan candidate
    return found


def load_manifests(state: Path) -> dict:
    out = state / "out"
    acc = {r["id"]: r for r in read_jsonl(out / "accepted.jsonl")}
    ref = {r["id"]: r for r in read_jsonl(out / "refused.jsonl")}
    att = {r["id"]: r for r in read_jsonl(out / "attempts.jsonl")}
    ind = {i: r for i, r in att.items() if i not in acc and i not in ref}
    deliv = read_jsonl(out / "delivered.jsonl")
    return {"accepted": acc, "refused": ref, "indeterminate": ind, "delivered": deliv}


def quarantined_ids(state: Path) -> list[int]:
    q = state / "evstore" / "events" / "quarantine.log"
    if not q.exists():
        return []
    ids = []
    for _, line in iter_records(q.read_bytes()):
        rid = record_id(line)
        if rid is not None:
            ids.append(rid)
    return ids


def check_inv1(state: Path) -> None:
    """INV-1: an effective SD_ONLY entry has a durable verifying SD copy on
    its CID, or its flash copy."""
    view = durable_view(state)
    idx_rel = "evstore/evq.idx"
    if not (state / idx_rel).exists():
        return
    segs, _, _ = parse_index(_durable_bytes(state, idx_rel, view))
    sd_state = (state / ".shim" / "sd_state").read_text().split() if (state / ".shim" / "sd_state").exists() else ["1", "c1d00001"]
    cur_cid = int(sd_state[1], 16)
    for s in segs.values():
        if s.state != "SD_ONLY":
            continue
        flash = state / "evstore" / "events" / f"ev-{s.seq:06d}.log"
        if flash.exists():
            continue
        cards = [state / "sdcard"] if s.cid == cur_cid else []
        cards.append(state / "cards" / f"{s.cid:08x}")
        ok = False
        for card in cards:
            for sub, name in (("events", s.primary), ("evq", s.mirror)):
                p = card / sub / name
                if p.exists():
                    data = _durable_bytes(state, str(p.relative_to(state)), view)
                    if len(data) == s.bytes and (zlib.crc32(data) & 0xFFFFFFFF) == s.crc:
                        ok = True
        if not ok:
            raise OracleFailure(f"INV-1: seg {s.seq} is SD_ONLY with no flash copy and no durable verifying SD copy")


def check_durability(dev: Device) -> None:
    st = dev.state
    m = load_manifests(st)
    delivered_ids = {d["id"] for d in m["delivered"]}
    reach = reachable_copies(st)
    missing = []
    for rid, r in m["accepted"].items():
        if rid in delivered_ids or rid in dev.allow_lost:
            continue
        if r["line_sha"] not in reach.get(rid, set()):
            missing.append(rid)
    if missing:
        raise OracleFailure(f"R-DUR: {len(missing)} accepted undelivered record(s) have no reachable durable intact copy: {sorted(missing)[:20]}")
    check_inv1(st)


def reconcile(dev: Device, allow_quarantine: set[int] | None = None) -> dict:
    """R-E2E (after recovery)."""
    st = dev.state
    m = load_manifests(st)
    acc, ref, ind, deliv = m["accepted"], m["refused"], m["indeterminate"], m["delivered"]
    by_id: dict[int, list[dict]] = {}
    for d in deliv:
        by_id.setdefault(d["id"], []).append(d)
    missing = sorted(i for i in acc if i not in by_id)
    mismatched = []
    fields = ["line_sha", "payload_sha", "meta_sha", "cmd_sha", "start_ms", "end_ms", "channel", "device", "tag"]
    for i, r in acc.items():
        for d in by_id.get(i, []):
            if any(d[f] != r[f] for f in fields):
                mismatched.append(i)
                break
    for i, r in ind.items():
        for d in by_id.get(i, []):
            if d["line_sha"] != r["line_sha"]:
                mismatched.append(i)
                break
    refused_delivered = sorted(i for i in ref if i in by_id)
    q = quarantined_ids(st)
    q_acc = sorted(set(q) & set(acc) - (allow_quarantine or set()))
    dup = sum(len(v) - 1 for v in by_id.values() if len(v) > 1)
    unknown = sorted(i for i in by_id if i not in acc and i not in ind and i not in ref)
    health = read_jsonl(st / "out" / "health.jsonl")
    order_ok = True
    firsts = []
    seen = set()
    for d in deliv:
        if d["id"] not in seen:
            seen.add(d["id"])
            firsts.append(d["id"])
    order_ok = firsts == sorted(firsts)
    return {
        "scenario": dev.scenario, "seed": dev.seed,
        "accepted": len(acc), "refused": len(ref), "indeterminate": len(ind),
        "delivered_records": len(deliv), "delivered_distinct": len(by_id),
        "missing_ids": missing, "mismatched_ids": sorted(set(mismatched)), "refused_delivered": refused_delivered,
        "quarantined_ids": sorted(set(q)), "quarantined_accepted": q_acc, "duplicate_count": dup,
        "unknown_delivered": unknown, "first_delivery_in_id_order": order_ok,
        "boots": dev.boots, "crashes": dev.crashes, "graceful_reboots": dev.graceful, "checkpoints": dev.checkpoints,
        "final_health": health[-1] if health else None,
    }


def assert_e2e(rec: dict, allow_missing: set[int] | None = None) -> None:
    miss = set(rec["missing_ids"]) - (allow_missing or set())
    problems = []
    if miss:
        problems.append(f"missing_ids={sorted(miss)[:20]} (n={len(miss)})")
    if rec["mismatched_ids"]:
        problems.append(f"mismatched_ids={rec['mismatched_ids'][:20]}")
    if rec["refused_delivered"]:
        problems.append(f"refused_delivered={rec['refused_delivered'][:20]}")
    if rec["quarantined_accepted"]:
        problems.append(f"quarantined∩accepted={rec['quarantined_accepted'][:20]}")
    if rec["unknown_delivered"]:
        problems.append(f"unknown_delivered={rec['unknown_delivered'][:20]}")
    if problems:
        raise OracleFailure("R-E2E: " + "; ".join(problems))


# ── evidence ───────────────────────────────────────────────────────────────
def evidence_dir(scenario: str, seed: int) -> Path | None:
    root = os.environ.get("EVQ_OUT")
    if not root:
        return None
    d = Path(root) / scenario / str(seed)
    d.mkdir(parents=True, exist_ok=True)
    return d


def write_evidence(dev: Device, rec: dict, extra: dict | None = None, media_before: dict | None = None) -> None:
    d = evidence_dir(dev.scenario, dev.seed)
    if d is None:
        return
    for name in ["accepted.jsonl", "refused.jsonl", "attempts.jsonl", "delivered.jsonl", "health.jsonl", "claims.jsonl"]:
        src = dev.state / "out" / name
        if src.exists():
            shutil.copyfile(src, d / name)
    ind = load_manifests(dev.state)["indeterminate"]
    (d / "indeterminate.jsonl").write_text("".join(json.dumps(v) + "\n" for v in ind.values()))
    q = dev.state / "evstore" / "events" / "quarantine.log"
    (d / "quarantined.jsonl").write_text("".join(json.dumps({"id": i}) + "\n" for i in quarantined_ids(dev.state)))
    out = dict(rec)
    if extra:
        out.update(extra)
    out["transforms"] = dev.transforms
    (d / "reconcile.json").write_text(json.dumps(out, indent=1, sort_keys=True, default=str))
    if media_before is not None:
        (d / "media_before.json").write_text(json.dumps(media_before, indent=1, sort_keys=True))
    (d / "media_after.json").write_text(json.dumps(media_hashes(dev.state), indent=1, sort_keys=True))
    ops = dev.state / ".shim" / "ops.jsonl"
    if ops.exists() and ops.stat().st_size < 64 * 1024 * 1024:
        shutil.copyfile(ops, d / "ops.jsonl")
    _ = q


def scratch_root() -> Path:
    base = os.environ.get("EVQ_TMPDIR")
    if base:
        Path(base).mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="evq-", dir=base))

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

    def fault_fired(self, name: str | None = None) -> bool:
        """Every armed spec fired (or, with `name`, that point's spec). A spec
        list "a:1:m,b:1:m" stays armed until all of them fired."""
        p = self.state / ".shim" / "fault_fired"
        if not p.exists() or self.armed is None:
            return False
        fired = {ln.split(":")[0] for ln in p.read_text().split()}
        want = [sp.split(":")[0] for sp in self.armed.split(",")]
        return name in fired if name is not None else all(w in fired for w in want)

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


# ── ARCH-INV: a retired spooled segment keeps a verified copy ─────────────
def check_arch_inv(dev: Device) -> dict:
    """Every segment that was SPOOLED (P line) and later retired (A line)
    without ever being re-imported (R line) must still have a byte-exact,
    CRC-verifying copy somewhere on SD (archive, primary, mirror, any card).
    Uses the cumulative index history the shim records, so compaction or
    boots cannot hide a retirement. Delivered ids are NOT exempt here: this
    is the redundancy check R-DUR deliberately does not make."""
    st = dev.state
    hist = st / ".shim" / "idx_history"
    if not hist.exists():
        return {"checked": 0}
    last_s: dict[int, tuple[int, int]] = {}
    spooled: set[int] = set()
    retired: dict[int, tuple[int, int]] = {}
    reimported: set[int] = set()
    for raw in hist.read_bytes().split(b"\n"):
        line = raw.decode("latin-1")
        m = re.match(r"^(.*) \*([0-9a-f]{8})$", line)
        if not m or (zlib.crc32(m.group(1).encode("latin-1")) & 0xFFFFFFFF) != int(m.group(2), 16):
            continue
        f = m.group(1).split(" ")
        try:
            if f[0] == "S":
                last_s[int(f[1])] = (int(f[5]), int(f[6], 16))
            elif f[0] == "P":
                spooled.add(int(f[1]))
            elif f[0] == "R":
                reimported.add(int(f[1]))
            elif f[0] == "A":
                q = int(f[1])
                if q in spooled and q in last_s:
                    retired[q] = last_s[q]
        except (ValueError, IndexError):
            continue
    want = {q: v for q, v in retired.items() if q not in reimported}
    if not want:
        return {"checked": 0}
    have: set[tuple[int, int]] = set()
    sizes = {v[0] for v in want.values()}
    for base in ["sdcard", "cards"]:
        d = st / base
        if not d.exists():
            continue
        for p in d.rglob("*"):
            if not p.is_file() or p.name.startswith(("bad-", "xlk-")):
                continue
            sz = p.stat().st_size
            if sz in sizes:
                have.add((sz, zlib.crc32(p.read_bytes()) & 0xFFFFFFFF))
    lost = sorted(q for q, v in want.items() if v not in have)
    if lost:
        raise OracleFailure(f"ARCH-INV: spooled segment(s) {lost[:10]} retired without any verified SD copy left")
    return {"checked": len(want)}


# ── C-19: state-aware last-good-copy and order oracle (+ C-18 cursor rule) ──
_IDX_MAIN = "evstore/evq.idx"
_FLASH_EV = re.compile(r"^evstore/events/ev-(\d+)\.log$")
_ARC = re.compile(r"^sdcard/archive/arc-(\d+)(?:-\d+)?\.log$")


def _idx_fields(line: str) -> list[str] | None:
    m = re.match(r"^(.*) \*([0-9a-f]{8})$", line)
    if not m or (zlib.crc32(m.group(1).encode("latin-1")) & 0xFFFFFFFF) != int(m.group(2), 16):
        return None
    return m.group(1).split(" ")


def _idx_apply(segs: dict, f: list[str]) -> None:
    """One CRC-valid index line onto a {seq: seg} map (documented format,
    same semantics as parse_index)."""
    k = f[0]
    try:
        if k == "S":
            q = int(f[1])
            segs[q] = {"seq": q, "first": int(f[2]), "last": int(f[3]), "state": "FLASH", "cid": None,
                       "primary": "", "mirror": ""}
        elif k == "P" and int(f[1]) in segs:
            s = segs[int(f[1])]
            s.update(state="SPOOLED", cid=f[2].lower(), primary=f[3], mirror=f[4])
        elif k == "O" and int(f[1]) in segs:
            segs[int(f[1])]["state"] = "SD_ONLY"
        elif k == "D" and int(f[1]) in segs:
            segs[int(f[1])]["state"] = "DELIVERED"
        elif k == "R" and int(f[1]) in segs:
            segs[int(f[1])]["state"] = "REIMPORT"
        elif k == "A":
            segs.pop(int(f[1]), None)
    except (ValueError, IndexError):
        pass


def last_good_copy_oracle(state: Path) -> dict:
    """C-19 over ops.jsonl + the index lines the shim logs (contract §5.3):

      (a) bad-* and xlk-* names are never unlinked;
      (b) a name that took part in a both_eio/ambiguous rename is never
          unlinked while its twin exists (the shim logs every unlink of an SD
          inode with other names as xlink_unlink, with the twins it found);
      (c) until an accepted id is ACKed with a durable cursor past it (or its
          segment is archived/retired: A line), at every trace position at
          least one copy containing it exists and last verified good: its
          flash segment, or an SD primary/mirror verified (P line) after its
          last write. Evaluated at every event that loses a copy.
      (d) a redundant committed name — a mirror after archive, a flash copy
          after reclaim, a primary/mirror after re-import — is removed only
          after its successor's verification op AND the durable index line
          appear earlier in the trace.

    Plus the C-18 cursor rule: a durable cursor commit never passes an
    accepted id that was not PUBACKed. State-aware exclusions (reported):
    ids of a segment that entered REIMPORT (the cursor moved without our ACK;
    R-DUR covers them), and records not stored through this trace's
    store_ok marks (fixtures, legacy imports). Positions are (seq, end
    offset) from the store_ok marks; the cursor is the conservative
    min(blob, legacy) of each NVS event."""
    events = read_jsonl(state / ".shim" / "ops.jsonl")
    viol: list[str] = []
    cur_viol: list[str] = []
    cnt = {"copy_loss_events": 0, "committed_removals": 0, "cursor_commits": 0, "xlink_unlink": 0,
           "accepted_tracked": 0, "legacy_firmware_ops": 0, "import_handover": 0}
    last_fp = {"name": ""}                  # last keeper fault point passed (stubs log them)
    scope = {"on": True}                     # rules apply to ops of this repo's firmware only

    def v(rule: str, i: int, msg: str) -> None:
        if not scope["on"]:
            return
        if rule == "cursor":
            cur_viol.append(f"C-18(cursor) @op{i}: {msg}")
        else:
            viol.append(f"C-19({rule}) @op{i}: {msg}")

    r_seqs = set()
    for e in events:
        if e.get("op") == "idx":
            f = _idx_fields(e.get("line", ""))
            if f and f[0] == "R":
                try:
                    r_seqs.add(int(f[1]))
                except (ValueError, IndexError):
                    pass

    cid = "c1d00001"
    segs: dict[int, dict] = {}
    idx_buf: dict[str, list] = {}
    pending_idx: list = []                  # (seq, kind) appended to main, not yet synced
    p_durable: dict[int, int] = {}          # seq -> op index where its P line became durable
    d_durable: dict[int, int] = {}          # seq -> op index where DELIVERED became durable (D line or cursor)
    r_pos: dict[int, int] = {}
    last_flash_sync = -1
    exists: set = set()                     # (medium_key, path) known to exist
    gone: set = set()                       # known removed
    verified: dict = {}                     # (cid, path) -> seq
    lastw: dict = {}                        # (cid, path) -> op index of last write/rename-to
    lastr: dict = {}                        # (cid, path) -> op index of last read-open
    ids_by_seq: dict[int, list] = {}
    pos: dict[int, tuple] = {}
    acked: set = set()
    covered: set = set()
    handover: set = set()
    cursor = (0, 0)

    def key(path: str):
        return ("flash", path) if path.startswith("evstore/") else (cid, path)

    def present(k) -> bool:
        return k in exists or k not in gone

    def flash_has(seq: int) -> bool:
        return present(("flash", f"evstore/events/ev-{seq:06d}.log"))

    def good_copy(seq: int) -> bool:
        if flash_has(seq):
            return True
        return any(sq == seq and present(k) for k, sq in verified.items())

    def check_c(seq: int, i: int, what: str) -> None:
        cnt["copy_loss_events"] += 1
        if seq in handover:
            return
        owed = [x for x in ids_by_seq.get(seq, []) if x not in covered]
        if owed and not good_copy(seq):
            v("c", i, f"{what}: segment {seq} holds {len(owed)} accepted un-ACKed id(s) (e.g. {owed[:3]}) "
                      f"and no verified copy is left")

    def seq_of_verified(k):
        return verified.get(k)

    def committed_owner(path: str):
        base = path.rsplit("/", 1)[-1]
        for s in segs.values():
            if s["cid"] is not None and s["cid"] != cid:
                continue
            if path.startswith("sdcard/events/") and s["primary"] == base:
                return s, "primary"
            if path.startswith("sdcard/evq/") and s["mirror"] == base:
                return s, "mirror"
        return None, None

    def check_d_sd(path: str, i: int) -> None:
        s, role = committed_owner(path)
        if s is None:
            return
        cnt["committed_removals"] += 1
        q = s["seq"]
        if s["state"] == "REIMPORT":
            if not (q in r_pos and last_flash_sync > r_pos[q]):
                v("d", i, f"{role} {path} of seg {q} removed after re-import without a synced flash append after its R line")
            return
        if role == "primary":
            v("d", i, f"committed primary {path} of seg {q} ({s['state']}) unlinked (only a rename into the archive or a re-import may retire it)")
            return
        if s["state"] in ("SPOOLED", "SD_ONLY") and not (q < cursor[0]):
            v("d", i, f"mirror {path} of undelivered seg {q} ({s['state']}) unlinked")
            return
        arcs = [k for k in exists if k[0] == cid and (m := _ARC.match(k[1])) and int(m.group(1)) == s["first"]]
        arc_ok = any(lastr.get(k, -1) > lastw.get(k, -1) for k in arcs)
        if q not in d_durable:
            v("d", i, f"mirror {path} of seg {q} removed before a durable DELIVERED index line/cursor")
        elif not arc_ok:
            v("d", i, f"mirror {path} of seg {q} removed before any verified archive copy (arc-{s['first']}*) was read back")

    def check_d_flash(seq: int, i: int) -> None:
        s = segs.get(seq)
        if s is None or s["state"] != "SPOOLED":
            return
        cnt["committed_removals"] += 1
        if seq not in p_durable:
            v("d", i, f"flash ev-{seq:06d} reclaimed before its P line was durable")
            return
        c = s["cid"] or cid
        for role, sub in (("primary", "events"), ("mirror", "evq")):
            k = (c, f"sdcard/{sub}/{s[role]}")
            if not (lastr.get(k, -1) > lastw.get(k, -1)):
                v("d", i, f"flash ev-{seq:06d} reclaimed before its {role} {k[1]} was read back after its last write")

    def sd_lost(k, i: int, what: str, imported: bool = False) -> None:
        q = verified.pop(k, None)
        if q is not None:
            if imported:
                # legacy import (e.g. after an index loss): every record of the
                # file was appended to the flash tail and synced before this
                # remove — the obligation moved to flash (R-DUR covers it)
                handover.add(q)
                cnt["import_handover"] += 1
                return
            check_c(q, i, what)

    def remove_path(path: str, i: int, what: str) -> None:
        k = key(path)
        base = path.rsplit("/", 1)[-1]
        if path.startswith("sdcard/") and base.startswith(("bad-", "xlk-")):
            v("a", i, f"{path} unlinked ({what})")
        if path.startswith("sdcard/"):
            check_d_sd(path, i)
        exists.discard(k)
        gone.add(k)
        if path.startswith("sdcard/"):
            imported = what == "remove" and last_fp["name"] == "import.after_fsync_before_sd_remove"
            last_fp["name"] = ""             # one import fault point covers exactly one remove
            sd_lost(k, i, f"{what} {path}", imported)
        else:
            m = _FLASH_EV.match(path)
            if m:
                q = int(m.group(1))
                check_d_flash(q, i)
                check_c(q, i, f"{what} {path}")

    def create(path: str, i: int) -> None:
        k = key(path)
        exists.add(k)
        gone.discard(k)
        lastw[k] = i

    def rename(a: str, b: str, i: int, link: bool = False) -> None:
        ka, kb = key(a), key(b)
        create(b, i)
        if link:
            return
        exists.discard(ka)
        gone.add(ka)
        q = verified.pop(ka, None)
        if q is not None:
            bb = b.rsplit("/", 1)[-1]
            if (b.startswith("sdcard/events/") or (b.startswith("sdcard/evq/") and bb.startswith("m-"))):
                verified[kb] = q               # the verified bytes keep being a queue copy
            else:
                check_c(q, i, f"rename {a} -> {b}")
        m = _FLASH_EV.match(a)
        if m:
            check_c(int(m.group(1)), i, f"rename {a}")

    def replay(lines: list) -> dict:
        out: dict[int, dict] = {}
        for ln in lines:
            f = _idx_fields(ln)
            if f:
                _idx_apply(out, f)
        return out

    def cursor_passed(p: tuple) -> bool:
        return p[0] < cursor[0] or (p[0] == cursor[0] and p[1] <= cursor[1])

    order: list = []                         # ids sorted by position for the cursor rule
    ptr = 0
    for i, e in enumerate(events):
        op = e.get("op")
        if not scope["on"]:
            cnt["legacy_firmware_ops"] += 1
        if op == "fp":
            last_fp["name"] = e.get("name", "")
            continue
        if op == "mark":
            w = e.get("what", "")
            if w.startswith("variant "):
                # ops of an old-API released firmware (rollback/fixture phases:
                # b3f9b8a, v2.2.3) are tracked for state but never judged
                scope["on"] = not w.split(" ", 1)[1].startswith("legacy")
            elif w.startswith("store_ok "):
                parts = w.split(" ")
                m = _FLASH_EV.match(parts[2]) if len(parts) >= 4 else None
                if m and int(parts[3]) >= 0:
                    rid, q = int(parts[1]), int(m.group(1))
                    ids_by_seq.setdefault(q, []).append(rid)
                    pos[rid] = (q, int(parts[3]))
                    order.append(rid)
                    cnt["accepted_tracked"] += 1
                    if q in handover:
                        pass
            elif w.startswith("puback "):
                rid = int(w.split(" ")[1])
                acked.add(rid)
                if rid in pos and cursor_passed(pos[rid]):
                    covered.add(rid)
            elif w.startswith("sdcmd "):
                m = re.search(r"cid=([0-9a-f]{8})", w)
                if m:
                    cid = m.group(1)
        elif op == "nvs":
            cands = [tuple(e[k]) for k in ("blob", "legacy") if e.get(k) and e[k][0] >= 0]
            cursor = min(cands) if cands else (0, 0)
            if e.get("kind") == "commit":
                cnt["cursor_commits"] += 1
                # store order == position order (the tail only grows or
                # rotates forward), so each id is examined once, when the
                # durable cursor first passes it
                while ptr < len(order) and cursor_passed(pos[order[ptr]]):
                    rid = order[ptr]
                    ptr += 1
                    p = pos[rid]
                    if rid in acked:
                        covered.add(rid)
                    elif p[0] not in r_seqs and p[0] not in handover:
                        v("cursor", i, f"durable cursor {cursor} passed accepted id {rid} at {p} before its PUBACK")
                for q in list(segs):
                    if q < cursor[0] and q not in d_durable:
                        d_durable[q] = i
        elif op == "idx":
            path = e.get("path", "")
            line = e.get("line", "")
            if e.get("load"):
                continue                         # handled with its idx_load header below
            if path != _IDX_MAIN:
                idx_buf.setdefault(path, []).append(line)
                continue
            f = _idx_fields(line)
            if not f:
                continue
            k = f[0]
            try:
                q = int(f[1]) if k in "SPODRA" and len(f) > 1 else None
            except ValueError:
                q = None
            if k == "S" and q is not None and q in segs and segs[q]["primary"]:
                old = segs[q]
                for role, sub in (("primary", "events"), ("mirror", "evq")):
                    verified.pop(((old["cid"] or cid), f"sdcard/{sub}/{old[role]}"), None)
            _idx_apply(segs, f)
            if k == "P" and q is not None and q in segs:
                s = segs[q]
                for role, sub in (("primary", "events"), ("mirror", "evq")):
                    verified[(s["cid"], f"sdcard/{sub}/{s[role]}")] = q
            if k == "R" and q is not None:
                r_pos[q] = i
                handover.add(q)
            if k == "A" and q is not None:
                covered.update(ids_by_seq.get(q, []))
            if q is not None:
                pending_idx.append((q, k))
        elif op == "idx_load":
            lines = []
            j = i + 1
            while j < len(events) and events[j].get("op") == "idx" and events[j].get("load"):
                lines.append(events[j].get("line", ""))
                j += 1
            segs = replay(lines)
            for s in segs.values():
                if s["state"] == "SPOOLED":
                    p_durable.setdefault(s["seq"], i)
                if s["state"] in ("DELIVERED",):
                    d_durable.setdefault(s["seq"], i)
                if s["state"] == "REIMPORT":
                    r_pos.setdefault(s["seq"], i)
                    handover.add(s["seq"])
            pending_idx.clear()
        elif op == "sync":
            path = e.get("path", "")
            if path == _IDX_MAIN:
                for q, k in pending_idx:
                    if k == "P":
                        p_durable[q] = i
                    elif k == "D":
                        d_durable.setdefault(q, i)
                pending_idx.clear()
            elif path.startswith("evstore/events/"):
                last_flash_sync = i
        elif op == "open_w":
            path = e.get("path", "")
            if "fail" in e:
                continue
            k = key(path)
            create(path, i)
            if path.startswith("sdcard/"):
                sd_lost(k, i, f"rewrite of {path}")
        elif op == "open_r":
            if "fail" not in e:
                lastr[key(e.get("path", ""))] = i
        elif op == "rename":
            if "fail" in e:
                continue
            a, b = e.get("from", ""), e.get("to", "")
            if a == "" or b == "":
                continue
            if b == _IDX_MAIN and a in idx_buf:
                segs = replay(idx_buf.pop(a))
                for s in segs.values():
                    if s["state"] == "SPOOLED":
                        p_durable.setdefault(s["seq"], i)
                pending_idx.clear()
            rename(a, b, i)
        elif op == "rename_inside":
            a, b = e.get("from", ""), e.get("to", "")
            if e.get("outcome") == "applied":
                rename(a, b, i)
            elif e.get("outcome") == "both":
                rename(a, b, i, link=True)
        elif op == "rename_both_eio":
            rename(e.get("from", ""), e.get("to", ""), i, link=True)
        elif op == "remove":
            if "fail" not in e:
                remove_path(e.get("path", ""), i, "remove")
        elif op == "remove_inside":
            if e.get("outcome") == "applied":
                remove_path(e.get("path", ""), i, "remove(crash inside)")
        elif op == "xlink_unlink":
            cnt["xlink_unlink"] += 1
            twins = e.get("twins") or []
            if twins:
                v("b", i, f"{e.get('path')} unlinked while its twin name(s) {twins} share the chain "
                          f"(FAT: the survivor's clusters are freed; modelled zero-fill of {e.get('zeroed')} B)")
            for t in twins:
                sd_lost(key(t), i, f"zero-fill of twin {t}")
    return {"violations": viol[:40], "n_violations": len(viol), "counts": cnt,
            "c18_cursor_violations": cur_viol[:40], "n_c18_cursor": len(cur_viol),
            "handover_seqs": sorted(handover)[:40], "rules": "C-19 a-d + C-18 cursor (tests/evq_host/evq_lib.py)"}

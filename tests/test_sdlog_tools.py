"""Sprint 2 host checkers for the sd_logger hardware rows (contract r4 H6):
tools/evq_hil/sdlog_check.py (parse, undump, continuity, LOG-REPLAY, LOG-LEDGER,
LOG-PRESERVE / REL-LOG, SL-BACKOFF, L-8, R(u)) and the hilpay pad twin.

The captures are produced by `Sim`, a small stand-in for the device that follows
sd_logger.c's writer rules (ring, whole-record pops, commit/rollback/quarantine,
rotate_files order) and prints the docs/sdlog-hil-trace.md grammar with \\r\\n and
unrelated interleaved console lines. PASS fixtures prove the checker accepts a
consistent run; every tamper fixture changes ONE thing and must FAIL or VOID
(exit 1 / 2), never PASS. The real-firmware cross-check is SLT-1 (tests/sdlog_host).
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
import hilpay  # noqa: E402
import sdlog_check as C  # noqa: E402

TOOL = ROOT / "tools" / "evq_hil" / "sdlog_check.py"
RUN = "s2-20260927-L1-01"
MIB = 1024 * 1024


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def lines_blob(tag: str, n: int, width: int = 90) -> bytes:
    line = (tag + "|" + "x" * (width - len(tag) - 2) + "\n").encode()
    return line * n


class Sim:
    """Device stand-in. Every writer action both changes the fake card and records the
    trace entry the firmware would record; console lines accumulate in `out`."""

    def __init__(self, files: dict[str, bytes] | None = None, run: str = RUN, pad: int = 40):
        self.card = {k: bytearray(v) for k, v in (files or {}).items()}
        self.run, self.pad = run, pad
        self.k = 100000
        self.seq = 1
        self.us = 5_000_000_000
        self.ents: list[str] = []
        self.out = bytearray()
        self.ring: list[bytes] = []
        self.open = False
        self.committed = 0
        self.quar = False
        self.acct = {k: 0 for k in C.ACCT_KEYS}

    # ---------------------------------------------------------------- console
    def line(self, s: str) -> None:
        self.out += (s + "\r\n").encode()

    def noise(self) -> None:
        self.line("\x1b[0;32mI (123456) wifi: unrelated console line\x1b[0m")

    def mark(self) -> int:
        return len(self.out)

    def ent(self, kind: str, *f, dt: int = 1000) -> None:
        self.us += dt
        self.ents.append(f"SLT_E {kind} {self.seq} {self.us} " + " ".join(str(x) for x in f))
        self.seq += 1

    def on(self) -> None:
        self.line(f"SLT_ON 2097152 {self.seq}")

    def drain(self, noise: bool = True) -> None:
        if not self.ents:
            self.line(f"SLT_DRAIN {self.seq} {self.seq - 1}")
            self.line(f"SLT_HDR {self.seq - 1} 0 {self.seq} {self.seq - 1} 0")
            return
        first = int(self.ents[0].split()[2])
        last = int(self.ents[-1].split()[2])
        self.line(f"SLT_DRAIN {first} {last}")
        for i, e in enumerate(self.ents):
            self.line(e)
            if noise and i == len(self.ents) // 2:
                self.noise()
        self.line(f"SLT_HDR {last} 0 {first} {last} {sum(len(e) for e in self.ents)}")
        self.ents = []

    def log_status(self) -> None:
        sz = len(self.card.get("ambyte.log", b""))
        self.line("SD log: writing")
        self.line(f" - file: /sdcard/logs/ambyte.log ({sz} bytes)")
        self.line(" - buffered: 0 bytes, dropped: 0 bytes")
        self.line(" - acct: " + json.dumps(self.acct, separators=(",", ":")))

    # ---------------------------------------------------------------- producer
    def record(self) -> bytes:
        k = self.k
        self.k += 1
        return C.record_bytes("2026-09-27 12:00:00", 1234567, self.run, k, hilpay.pad(self.run, k, self.pad))

    def push(self, rec: bytes | None = None, dropped: bool = False) -> bytes:
        rec = rec or self.record()
        syn = 1 if C.SYN_TAG in rec else 0
        self.ent("P", len(rec), sha(rec), "dropped" if dropped else "pushed", syn, base64.b64encode(rec).decode())
        if dropped:
            self.acct["ring"] += len(rec)
        else:
            self.ring.append(rec)
        return rec

    def pop(self, cap: int = 512) -> bytes:
        out = b""
        while self.ring and len(out) + len(self.ring[0]) <= cap:
            out += self.ring.pop(0)
        if out:
            self.ent("POP", len(out))
        return out

    # ---------------------------------------------------------------- writer
    @property
    def size(self) -> int:
        return len(self.card["ambyte.log"])

    def open_(self) -> None:
        f = self.card.setdefault("ambyte.log", bytearray())
        torn = 1 if f and f[-1] != 0x0A else 0
        self.ent("OPEN", "ambyte.log", len(f), torn)
        if torn:
            self.quar = True
            self.acct["torn"] += 1
        self.open = True
        self.committed = len(f)

    def write(self, chunk: bytes, written: int | None = None) -> None:
        w = len(chunk) if written is None else written
        self.ent("WR", len(chunk), w, self.size)
        self.card["ambyte.log"] += chunk[:w]
        if w < len(chunk):
            self.acct["unwr"] += len(chunk) - w

    def commit(self) -> None:
        if self.open and self.size != self.committed:
            self.ent("COMMIT", self.size)
            self.committed = self.size

    def rb(self, result: str = "ok") -> None:
        since = self.size - self.committed
        self.ent("RB", self.committed, since, result)
        if result == "ok":
            self.acct["rb"] += since
            del self.card["ambyte.log"][self.committed:]
            return
        self.acct["indet"] += since
        if result == "quar_fsync":
            del self.card["ambyte.log"][self.committed:]
        self.quar = True
        self.open = False

    def close(self) -> None:
        if self.open:
            self.ent("CLOSE", "ok")
            self.open = False

    def drop(self, chunk: bytes, why: str = "rotate_blocked") -> None:
        self.ent("DROP", why, len(chunk))
        self.acct["rot" if why == "rotate_blocked" else "unav"] += len(chunk)

    def rotate(self, ok: bool = True, fail_op: str | None = None, fail_src: int | None = None,
               dt: int = 1000) -> None:
        self.commit()
        self.close()
        names = C.LOG_NAMES
        if fail_op == "remove":
            self.line("HIL_FAULT fired kind=injected_errno mode=eio writer=sdlog op=remove "
                      "path=/sdcard/logs/ambyte.5.log errno=5 nth=1 slot=A")
            self.ent("ROT", "fail", "remove", 5, dt=dt)
        else:
            old = self.card.pop("ambyte.5.log", None)
            if old:
                self.acct["evict"] += len(old)
            stop = -1 if ok else fail_src
            for i in range(4, stop, -1):
                if names[i] in self.card:
                    self.card[names[i + 1]] = self.card.pop(names[i])
            if ok:
                self.ent("ROT", "ok", "-", 0, dt=dt)
                self.quar = False
            else:
                self.line("HIL_FAULT fired kind=injected_errno mode=eio writer=sdlog op=rename "
                          f"path=/sdcard/logs/{names[fail_src]} errno=5 nth=1 slot=A")
                self.ent("ROT", "fail", "rename", 5, dt=dt)
        self.open_()

    def service(self) -> None:
        if not self.open:
            self.open_()
        chunk = self.pop()
        if chunk:
            self.write(chunk)

    def emit(self, n: int) -> None:
        for i in range(n):
            self.push()
            if i % 2 == 1:
                self.service()
        while self.ring:
            self.service()
        self.commit()

    # ---------------------------------------------------------------- quiesce / inventory / dump
    def quiesce(self, inv: bool = True, dumps: tuple[str, ...] = ()) -> tuple[int, int]:
        lo = self.mark()
        for name in dumps:
            self.line(f"ambyte> evq_hil sdlog_dump {name}")
        while self.ring:
            self.service()
        self.commit()
        self.close()
        self.ent("QUI", "paused")
        self.line("SDL_Q paused")
        self.noise()
        if inv:
            for name in sorted(self.card):
                b = bytes(self.card[name])
                ls = b.split(b"\n")
                torn = 1 if b and not b.endswith(b"\n") else 0
                self.line(f"SDL_FF {name} {len(b)} {sha(b)} {b.count(b'\\n')} {torn} "
                          f"{max((len(x) + 1 for x in ls), default=0)}")
            self.line(f"SDL_STATE ambyte.log {self.committed} {int(self.quar)} 0")
            self.line(f"SDL_INV_END {len(self.card)}")
        for name in dumps:
            b = bytes(self.card[name])
            for off in range(0, len(b), 3072):
                self.line(f"SDL_B64 {off} {base64.b64encode(b[off:off + 3072]).decode()}")
            self.line(f"SDL_DUMP_END 0 {len(b)} {sha(b)} {sha(b)}")
        self.line("SDL_Q resumed")
        self.ent("QUI", "resumed")
        self.open_()
        return lo, self.mark()

    def cut(self, rng: tuple[int, int]) -> str:
        return f"{rng[0]}:{rng[1]}"


def chain(n_rot: int = 5, active: bytes | None = None) -> dict[str, bytes]:
    files = {"ambyte.log": active if active is not None else lines_blob("A0", 20)}
    for i in range(1, n_rot + 1):
        files[f"ambyte.{i}.log"] = lines_blob(f"R{i}", 10 + i)
    return files


def exports_of(files: dict[str, bytes]) -> list[dict]:
    return [{"name": k, "size": len(v), "sha256": sha(v)} for k, v in files.items()]


def standard_window(sim: Sim, body, i1_dumps=("ambyte.log",)) -> dict:
    """L-row protocol: drain, on, log_status, I0 (inventory + dump of ambyte.log), body,
    I1 (inventory + dumps), log_status, final drain."""
    sim.drain()
    sim.on()
    sim.log_status()
    i0 = sim.quiesce(inv=True, dumps=("ambyte.log",))
    sim.drain()
    body(sim)
    sim.drain()
    i1 = sim.quiesce(inv=True, dumps=tuple(i1_dumps))
    sim.log_status()
    sim.drain()
    return {"i0": i0, "i1": i1}


def run_replay(sim: Sim, w: dict, exports: list[dict] | None):
    cp = C.parse_capture(bytes(sim.out))
    return C.replay(cp, w["i0"], w["i1"], exports)


def run_ledger(sim: Sim, w: dict, exports: list[dict] | None, data: bytes | None = None):
    cp = C.parse_capture(bytes(sim.out) if data is None else data)
    return C.ledger(cp, w["i0"], w["i1"], exports)


# =========================================================================== hilpay / rlen

class PadTwin(unittest.TestCase):
    def test_pad_is_exactly_the_payload_pad(self):
        for run, k, n in (("r", 0, 0), (RUN, 100000, 160), ("A.b_c-9", 4294967295, 7)):
            p = hilpay.pad(run, k, n)
            self.assertEqual(len(p), n)
            self.assertEqual(hilpay.payload(run, k, n), '{"evq_hil":"%s","k":%u,"pad":"%s"}' % (run, k, p))
            self.assertEqual(hilpay.sdlog_text(run, k, n), f"{run} {k} {p}")
        with self.assertRaises(ValueError):
            hilpay.pad("bad run", 1, 1)


class Rlen(unittest.TestCase):
    def test_r_matches_the_framed_record(self):
        for u in (7, 8, 9):
            for run in (RUN, "s2-x"):
                rec = C.record_bytes("2026-09-27 12:00:00", int("1" * u), run, 100000, hilpay.pad(run, 100000, 160))
                self.assertEqual(C.rlen(run, u)["R"], len(rec), (u, run))
                self.assertLessEqual(len(rec), C.LINE_MAX)

    def test_planning_numbers_and_cli(self):
        r = C.rlen(RUN, 7, s_active=500_000, rho=4000.0)
        self.assertEqual(r["B_rot"], MIB - 500_000 + 256)
        self.assertEqual(r["L0"]["N"], math.ceil(1.2 * r["B_rot"] / r["R"]))      # N(t_rot0), t_rot0 = B/(f R)
        self.assertEqual(r["N_rot"], math.ceil(1.2 * (r["B_rot"] / 4000.0) * 20))
        self.assertEqual(r["L5"]["T_b_s"], 600.0)
        p = subprocess.run([sys.executable, TOOL, "rlen", "--run", RUN, "--u", "7"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(json.loads(p.stdout)["R"], 229)
        p = subprocess.run([sys.executable, TOOL, "rlen", "--run", "x" * 40, "--u", "9", "--pad", "200"],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 1)          # > 256 B record: planning refuses


# =========================================================================== parse / undump / continuity

class Parse(unittest.TestCase):
    def test_parse_robust_to_noise_crlf_ansi_and_ranges(self):
        s = Sim(chain(2))
        w = standard_window(s, lambda s: s.emit(12))
        blob = bytes(s.out)
        self.assertIn(b"\r\n", blob)
        cp = C.parse_capture(blob)
        self.assertEqual(cp["malformed"], [])
        self.assertEqual(cp["orphans"], [])
        self.assertTrue(all(d["complete"] for d in cp["drains"]))
        self.assertEqual(len(cp["log_status"]), 2)
        self.assertEqual(cp["log_status"][0]["file_bytes"], len(chain(2)["ambyte.log"]))
        self.assertEqual(len([x for x in cp["sessions"] if x["state"] == "paused"]), 2)
        self.assertEqual([d["name"] for d in cp["dumps"]], ["ambyte.log", "ambyte.log"])
        # a byte range restricts the parse to that part of the capture
        part = C.parse_capture(blob, *w["i1"])
        self.assertEqual(len(part["sessions"]), 1)
        self.assertEqual(part["drains"], [])

    def test_interleaved_fragment_breaks_one_line_and_is_caught(self):
        s = Sim(chain(1))
        s.on()
        s.emit(4)
        s.drain(noise=False)
        blob = bytes(s.out)
        i = blob.index(b"SLT_E POP")
        glued = blob[:i] + b"I (5) x: yy" + blob[i:]          # another task's text glued in front
        cont = C.continuity(C.parse_capture(glued))
        self.assertEqual(cont["status"], C.VOID)

    def test_fired_and_witness_lines(self):
        s = Sim()
        s.line("HIL_FAULT fired kind=short_write mode=short writer=sdlog op=write path=/sdcard/logs/ambyte.log "
               "errno=0 nth=1 slot=B")
        chunk = b"abc\n"
        s.line(f"SLT_RESET_WITNESS /sdcard/logs/ambyte.log 10 4 2 {sha(chunk)} {base64.b64encode(chunk).decode()}")
        cp = C.parse_capture(bytes(s.out))
        self.assertEqual(cp["fired"][0]["slot"], "B")
        self.assertEqual(cp["fired"][0]["op"], "write")
        self.assertEqual(cp["witness"][0]["data"], chunk)


class Undump(unittest.TestCase):
    def cli(self, *args):
        return subprocess.run([sys.executable, TOOL, *map(str, args)], capture_output=True, text=True)

    def test_undump_reconstructs_bytes_private(self):
        big = os.urandom(3072 * 2 + 17)
        s = Sim({"ambyte.log": big})
        s.quiesce(inv=True, dumps=("ambyte.log",))
        with tempfile.TemporaryDirectory() as d:
            cap = Path(d) / "cap.log"
            cap.write_bytes(bytes(s.out))
            out = Path(d) / "a.bin"
            r = self.cli("undump", "--capture", cap, "--index", "0", "--out", out)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertEqual(out.read_bytes(), big)
            self.assertEqual(out.stat().st_mode & 0o777, 0o600)
            self.assertTrue(json.loads(r.stdout)["dumps"][0]["sdl_ff_match"])

    def test_undump_rejects_gap_and_bad_sha(self):
        big = os.urandom(3072 * 3)
        s = Sim({"ambyte.log": big})
        s.quiesce(inv=False, dumps=("ambyte.log",))
        blob = bytes(s.out)
        lines = blob.split(b"\r\n")
        gap = b"\r\n".join(x for x in lines if not x.startswith(b"SDL_B64 3072 "))
        cp = C.parse_capture(gap)
        self.assertFalse(cp["dumps"][0]["ok"])
        flipped = blob.replace(b"SDL_DUMP_END 0", b"SDL_DUMP_END 0", 1)
        i = flipped.index(b"SDL_B64 0 ") + 12
        flipped = flipped[:i] + (b"A" if flipped[i:i + 1] != b"A" else b"B") + flipped[i + 1:]
        cp = C.parse_capture(flipped)
        self.assertFalse(cp["dumps"][0]["ok"])


class Continuity(unittest.TestCase):
    def base(self) -> Sim:
        s = Sim(chain(1))
        s.on()
        s.emit(6)
        s.drain()
        s.emit(6)
        s.drain()
        return s

    def test_pass(self):
        self.assertEqual(C.continuity(C.parse_capture(bytes(self.base().out)))["status"], C.PASS)

    def test_gap_voids(self):
        blob = bytes(self.base().out)
        lines = blob.split(b"\r\n")
        victim = next(i for i, x in enumerate(lines) if x.startswith(b"SLT_E POP"))
        blob2 = b"\r\n".join(lines[:victim] + lines[victim + 1:])
        self.assertEqual(C.continuity(C.parse_capture(blob2))["status"], C.VOID)

    def test_lost_voids(self):
        blob = bytes(self.base().out)
        i = blob.index(b"SLT_HDR ")
        head, rest = blob[:i], blob[i:]
        f = rest.split(b" ", 3)
        rest = b" ".join([f[0], f[1], b"3", f[3]])
        self.assertEqual(C.continuity(C.parse_capture(head + rest))["status"], C.VOID)

    def test_sha_mismatch_voids(self):
        blob = bytes(self.base().out)
        i = blob.index(b"SLT_E P ")
        j = blob.index(b" pushed", i)
        k = j - 64
        blob2 = blob[:k] + (b"0" * 64 if blob[k:j] != b"0" * 64 else b"1" * 64) + blob[j:]
        self.assertEqual(C.continuity(C.parse_capture(blob2))["status"], C.VOID)

    def test_fill_over_half_stops_and_cli_exit_codes(self):
        blob = bytes(self.base().out)
        i = blob.index(b"SLT_HDR ")
        end = blob.index(b"\r\n", i)
        f = blob[i:end].split(b" ")
        f[5] = str(1_100_000).encode()
        blob2 = blob[:i] + b" ".join(f) + blob[end:]
        self.assertEqual(C.continuity(C.parse_capture(blob2))["status"], C.STOP)
        with tempfile.TemporaryDirectory() as d:
            for data, rc in ((blob, 0), (blob2, 3)):
                cap = Path(d) / "c.log"
                cap.write_bytes(data)
                r = subprocess.run([sys.executable, TOOL, "continuity", "--capture", cap], capture_output=True, text=True)
                self.assertEqual(r.returncode, rc, r.stdout)

    def test_unframed_record_fails(self):
        s = Sim()
        s.on()
        s.push(b"two\nrecords\n")
        s.drain()
        self.assertEqual(C.continuity(C.parse_capture(bytes(s.out)))["status"], C.FAIL)


# =========================================================================== replay / ledger

class Replay(unittest.TestCase):
    def assertPass(self, r):
        self.assertEqual(r["status"], C.PASS, json.dumps(r.get("reasons"), indent=1))

    def test_simple_append(self):
        s = Sim(chain(2))
        w = standard_window(s, lambda s: s.emit(30))
        r = run_replay(s, w, exports_of(chain(2)))
        self.assertPass(r)
        act = [x for x in r["S"] if x["name"] == "ambyte.log"][0]
        self.assertTrue(act["dump_verified"])
        self.assertEqual(act["i0_size"], len(chain(2)["ambyte.log"]))
        led = run_ledger(s, w, exports_of(chain(2)))
        self.assertPass(led)
        self.assertEqual(led["buckets"]["surviving"], led["window_bytes"])
        self.assertGreater(led["window_bytes"], 30 * 100)

    def test_rotation_with_eviction(self):
        files = chain(5)
        s = Sim(files)

        def body(s):
            s.emit(10)
            s.rotate(ok=True)
            s.emit(10)
        w = standard_window(s, body, i1_dumps=("ambyte.1.log", "ambyte.log"))
        r = run_replay(s, w, exports_of(files))
        self.assertPass(r)
        self.assertEqual([e["origin"] for e in r["E"]], ["I0:ambyte.5.log"])
        self.assertEqual(r["E"][0]["export"], "ambyte.5.log")
        led = run_ledger(s, w, exports_of(files))
        self.assertPass(led)
        self.assertEqual(led["deltas"]["evict"], len(files["ambyte.5.log"]))
        # the eviction without a prior export FAILs
        r = run_replay(s, w, [x for x in exports_of(files) if x["name"] != "ambyte.5.log"])
        self.assertEqual(r["status"], C.FAIL)

    def test_rb_ok_after_fsync_failure(self):
        s = Sim(chain(1))

        def body(s):
            s.emit(6)
            s.push(), s.push()
            s.write(s.pop())
            s.rb("ok")                        # commit fsync EIO -> rolled back to the commit
            s.emit(6)
        w = standard_window(s, body)
        self.assertPass(run_replay(s, w, []))
        led = run_ledger(s, w, [])
        self.assertPass(led)
        self.assertGreater(led["buckets"]["rolled_back"], 0)
        self.assertEqual(led["deltas"]["rb"], led["buckets"]["rolled_back"])

    def test_short_write_then_rb(self):
        s = Sim(chain(1))

        def body(s):
            s.emit(4)
            s.push(), s.push(), s.push()
            c = s.pop()
            s.write(c, written=len(c) // 2)
            s.rb("ok")
            s.emit(4)
        w = standard_window(s, body)
        led = run_ledger(s, w, [])
        self.assertPass(led)
        self.assertGreater(led["buckets"]["unwritten"], 0)
        self.assertGreater(led["buckets"]["rolled_back"], 0)

    def _quarantine(self, result: str):
        files = chain(5)
        s = Sim(files)

        def body(s):
            s.emit(4)
            s.push(), s.push()
            s.write(s.pop())
            s.rb(result)                      # quarantined, handle closed inside rollback()
            s.open_()                         # writer reopens ...
            s.rotate(ok=True)                 # ... and the quarantine forces a rotation
            s.emit(4)
        w = standard_window(s, body, i1_dumps=("ambyte.1.log", "ambyte.log"))
        return s, w, files

    def test_quar_trunc_keeps_the_tail(self):
        s, w, files = self._quarantine("quar_trunc")
        r = run_replay(s, w, exports_of(files))
        self.assertPass(r)
        q = [x for x in r["S"] if x["name"] == "ambyte.1.log"][0]
        self.assertTrue(q["quarantined"] and q["dump_verified"])
        led = run_ledger(s, w, exports_of(files))
        self.assertPass(led)
        self.assertGreater(led["indeterminate_present"], 0)
        self.assertEqual(led["buckets"]["indeterminate_absent"], 0)

    def test_quar_fsync_truncates(self):
        s, w, files = self._quarantine("quar_fsync")
        led = run_ledger(s, w, exports_of(files))
        self.assertPass(led)
        self.assertGreater(led["buckets"]["indeterminate_absent"], 0)
        self.assertEqual(led["indeterminate_present"], 0)

    def test_rot_fail_rename_mid_chain(self):
        files = chain(5)
        s = Sim(files)

        def body(s):
            s.emit(6)
            s.rotate(ok=False, fail_op="rename", fail_src=2)    # rm 5, 4->5, 3->4 applied; 2->3 fails
            s.emit(6)
        w = standard_window(s, body)
        r = run_replay(s, w, exports_of(files))
        self.assertPass(r)
        names = {x["name"]: x["origin"] for x in r["S"]}
        self.assertEqual(names["ambyte.5.log"], "I0:ambyte.4.log")
        self.assertEqual(names["ambyte.4.log"], "I0:ambyte.3.log")
        self.assertNotIn("ambyte.3.log", names)
        self.assertEqual(names["ambyte.2.log"], "I0:ambyte.2.log")
        self.assertPass(run_ledger(s, w, exports_of(files)))
        # without the fired line the failing step is unknowable: VOID, never a guess
        blob = b"\r\n".join(x for x in bytes(s.out).split(b"\r\n") if b"HIL_FAULT fired" not in x)
        cp = C.parse_capture(blob)
        self.assertEqual(C.replay(cp, w["i0"], w["i1"], exports_of(files))["status"], C.VOID)

    def test_rotate_blocked_drop_is_ledgered(self):
        s = Sim(chain(1))

        def body(s):
            s.emit(4)
            s.push(), s.push()
            s.drop(s.pop(), "rotate_blocked")
            s.emit(2)
        w = standard_window(s, body)
        led = run_ledger(s, w, [])
        self.assertPass(led)
        self.assertEqual(led["deltas"]["rot"], led["buckets"]["dropped_rotate_blocked"])

    # ---------------------------------------------------------------- tampering
    def good(self):
        s = Sim(chain(2))
        w = standard_window(s, lambda s: s.emit(20))
        return s, w

    @staticmethod
    def retamper(blob: bytes, rng: tuple[int, int], flip_at: int) -> tuple[bytes, tuple[int, int]]:
        """Flip one byte of the I1 dump and re-emit consistent SDL_B64 / SDL_DUMP_END lines
        (the device-side hashes agree with the forged bytes: only the host comparison can tell)."""
        a, b = rng
        seg = blob[a:b]
        ls = seg.split(b"\r\n")
        b64 = [i for i, x in enumerate(ls) if x.startswith(b"SDL_B64 ")]
        data = bytearray(b"".join(base64.b64decode(ls[i].split(b" ")[2]) for i in b64))
        data[flip_at] ^= 0x01
        new = [f"SDL_B64 {o} {base64.b64encode(bytes(data[o:o + 3072])).decode()}".encode()
               for o in range(0, len(data), 3072)]
        end = next(i for i, x in enumerate(ls) if x.startswith(b"SDL_DUMP_END"))
        tail = f"SDL_DUMP_END 0 {len(data)} {sha(bytes(data))} {sha(bytes(data))}".encode()
        seg2 = b"\r\n".join(ls[:b64[0]] + new + [tail] + ls[end + 1:])
        return blob[:a] + seg2 + blob[b:], (a, b + len(seg2) - len(seg))

    def test_tampered_i1_dump_fails(self):
        s, w = self.good()
        for flip_at, prefix_ok in ((-5, True), (3, False)):     # appended part, then the I0 prefix
            blob2, i1 = self.retamper(bytes(s.out), w["i1"], flip_at)
            r = C.replay(C.parse_capture(blob2), w["i0"], i1, [])
            self.assertEqual(r["status"], C.FAIL, r["reasons"])
            dump = [c for c in r["checks"] if c.get("what") == "dump" and c["i1"]][0]
            self.assertEqual((dump["prefix_ok"], dump["append_ok"]), (prefix_ok, not prefix_ok))

    def test_tampered_writer_event_voids(self):
        s, w = self.good()
        blob = bytes(s.out)
        i = blob.index(b"SLT_E WR ", w["i0"][1])
        j = blob.index(b"\r\n", i)
        f = blob[i:j].split(b" ")
        f[-1] = str(int(f[-1]) + 1).encode()     # WR `before` off by one
        blob2 = blob[:i] + b" ".join(f) + blob[j:]
        cp = C.parse_capture(blob2)
        self.assertEqual(C.replay(cp, w["i0"], (w["i1"][0] + 1, w["i1"][1] + 1), [])["status"], C.VOID)

    def test_missing_i0_dump_voids(self):
        s = Sim(chain(1))
        s.drain()
        s.on()
        s.log_status()
        i0 = s.quiesce(inv=True, dumps=())
        s.emit(5)
        s.drain()
        i1 = s.quiesce(inv=True, dumps=("ambyte.log",))
        s.log_status()
        s.drain()
        r = C.replay(C.parse_capture(bytes(s.out)), i0, i1, [])
        self.assertEqual(r["status"], C.VOID)

    def test_s_file_without_i1_dump_voids(self):
        files = chain(5)
        s = Sim(files)

        def body(s):
            s.emit(10)
            s.rotate(ok=True)
            s.emit(10)
        w = standard_window(s, body, i1_dumps=("ambyte.log",))     # ambyte.1.log (touched) not dumped
        self.assertEqual(run_replay(s, w, exports_of(files))["status"], C.VOID)

    def test_ledger_counter_tamper_fails(self):
        s, w = self.good()
        blob = bytes(s.out)
        i = blob.rindex(b'"rb":0')
        blob2 = blob[:i] + b'"rb":7' + blob[i + 6:]
        self.assertEqual(run_ledger(s, w, [], data=blob2)["status"], C.FAIL)
        i = blob.rindex(b'"unwr":0')
        blob3 = blob[:i] + b'"unwr":1' + blob[i + 8:]
        self.assertEqual(run_ledger(s, w, [], data=blob3)["status"], C.FAIL)

    def test_ledger_ring_drop_must_match(self):
        s = Sim(chain(1))

        def body(s):
            s.emit(2)
            s.push(dropped=True)
            s.emit(2)
        w = standard_window(s, body)
        led = run_ledger(s, w, [])
        self.assertEqual(led["status"], C.PASS, led["reasons"])
        self.assertGreater(led["producer_dropped"], 0)
        blob = bytes(s.out)
        i = blob.rindex(b'"ring":')
        j = blob.index(b",", i)
        blob2 = blob[:i] + b'"ring":0' + blob[j:]
        self.assertEqual(run_ledger(s, w, [], data=blob2)["status"], C.FAIL)

    def test_cli_verify_and_ledger_exit_codes(self):
        files = chain(5)
        s = Sim(files)

        def body(s):
            s.emit(8)
            s.rotate(ok=True)
            s.emit(8)
        w = standard_window(s, body, i1_dumps=("ambyte.1.log", "ambyte.log"))
        with tempfile.TemporaryDirectory() as d:
            cap = Path(d) / "cap.log"
            cap.write_bytes(bytes(s.out))
            ex = Path(d) / "ex.json"
            ex.write_text(json.dumps(exports_of(files)))
            for cmd in ("verify", "ledger"):
                r = subprocess.run([sys.executable, TOOL, cmd, "--capture", cap, "--i0", s.cut(w["i0"]),
                                    "--i1", s.cut(w["i1"]), "--exports", ex], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout[-2000:])
            ex.write_text("[]")
            r = subprocess.run([sys.executable, TOOL, "verify", "--capture", cap, "--i0", s.cut(w["i0"]),
                                "--i1", s.cut(w["i1"]), "--exports", ex], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)


class ReplayModelApi(unittest.TestCase):
    def test_importable_replay_model(self):
        s = Sim({"ambyte.log": b"old\n"})
        s.open_()
        s.emit(5)
        s.rotate(ok=True)
        s.emit(3)
        pred, ev = C.replay_model({"ambyte.log": b"old\n"}, s.ents)
        self.assertEqual(pred, {k: bytes(v) for k, v in s.card.items()})
        self.assertEqual(ev, [])
        bad = [e.replace(" WR ", " WR ", 1) for e in s.ents]
        i = next(i for i, e in enumerate(bad) if " WR " in e)
        f = bad[i].split(" ")
        f[-1] = str(int(f[-1]) + 3)
        bad[i] = " ".join(f)
        with self.assertRaises(ValueError):
            C.replay_model({"ambyte.log": b"old\n"}, bad)


# =========================================================================== preserve / REL-LOG

def inv(files: dict[str, bytes], prefixes: dict[str, int] | None = None) -> dict:
    out = {}
    for k, v in files.items():
        out[k] = {"size": len(v), "sha256": sha(v)}
        if prefixes and k in prefixes:
            out[k]["prefixes"] = {str(prefixes[k]): sha(v[:prefixes[k]])}
    return {"t": "inv", "files": out}


class Preserve(unittest.TestCase):
    def test_append_rotate_evict_exported(self):
        f0 = chain(5)
        ev = [inv(f0)] + [{"t": "export", "name": k, "size": len(v), "sha256": sha(v)} for k, v in f0.items()]
        grown = dict(f0, **{"ambyte.log": f0["ambyte.log"] + b"more\n"})
        ev.append(inv(grown, {"ambyte.log": len(f0["ambyte.log"])}))
        ev.append({"t": "rot", "ok": True, "evict_delta": len(f0["ambyte.5.log"])})
        shifted = {"ambyte.log": b"new\n"}
        for i in range(1, 5):
            shifted[f"ambyte.{i + 1}.log"] = grown["ambyte.log"] if i == 0 else f0[f"ambyte.{i}.log"]
        shifted["ambyte.1.log"] = grown["ambyte.log"]
        ev.append(inv(shifted))
        r = C.preserve(ev)
        self.assertEqual(r["status"], C.PASS, r["reasons"])
        self.assertEqual(r["finalized_unexported"], ["ambyte.log@0"])     # new ambyte.1.log awaits its export
        self.assertEqual(len(r["evictions"]), 1)

    def test_unbacked_eviction_fails(self):
        f0 = chain(5)
        ev = [inv(f0), {"t": "rot", "ok": True, "evict_delta": len(f0["ambyte.5.log"])}]
        self.assertEqual(C.preserve(ev)["status"], C.FAIL)

    def test_prefix_change_and_vanish_fail(self):
        f0 = chain(2)
        changed = dict(f0, **{"ambyte.log": b"X" + f0["ambyte.log"][1:] + b"tail\n"})
        self.assertEqual(C.preserve([inv(f0), inv(changed, {"ambyte.log": len(f0["ambyte.log"])})])["status"], C.FAIL)
        gone = {k: v for k, v in f0.items() if k != "ambyte.2.log"}
        self.assertEqual(C.preserve([inv(f0), inv(gone)])["status"], C.FAIL)
        edited = dict(f0, **{"ambyte.1.log": f0["ambyte.1.log"] + b"x\n"})
        self.assertEqual(C.preserve([inv(f0), inv(edited)])["status"], C.FAIL)

    def test_evict_delta_mismatch_fails(self):
        f0 = chain(5)
        ev = [inv(f0), {"t": "export", "name": "ambyte.5.log", "size": len(f0["ambyte.5.log"]),
                        "sha256": sha(f0["ambyte.5.log"])},
              {"t": "rot", "ok": True, "evict_delta": 1}]
        self.assertEqual(C.preserve(ev)["status"], C.FAIL)


class RelLog(unittest.TestCase):
    EXP = {"ambyte.5.log": 1000, "ambyte.4.log": 1100, "ambyte.3.log": 1200, "ambyte.2.log": 1300,
           "ambyte.1.log": 1400}

    def st(self, evict, fb):
        return {"kind": "status", "evict": evict, "file_bytes": fb}

    def test_prefix_sums_across_boots(self):
        caps = [self.st(0, 500), self.st(0, 900), self.st(1000, 20), self.st(1000, 400), {"kind": "boot"},
                self.st(0, 420), self.st(0, 900)]
        r = C.rel_log(self.EXP, caps)
        self.assertEqual(r["status"], C.PASS, r["reasons"])
        self.assertEqual(r["j"], 1)

    def test_across_reset_rotation_term(self):
        caps = [self.st(1000, 1_040_000), {"kind": "boot"}, self.st(0, 3000), self.st(0, 9000)]
        r = C.rel_log(self.EXP, caps)
        self.assertEqual(r["status"], C.PASS, r["reasons"])
        self.assertEqual(r["j"], 2)
        # the same shrink with a post-boot evict already counted is not double-counted
        caps = [self.st(1000, 1_040_000), {"kind": "boot"}, self.st(1100, 3000)]
        self.assertEqual(C.rel_log(self.EXP, caps)["j"], 2)

    def test_unbacked_value_fails_and_j3_stops(self):
        self.assertEqual(C.rel_log(self.EXP, [self.st(999, 10)])["status"], C.FAIL)
        caps = [self.st(1000, 1), self.st(2100, 1), self.st(3300, 1)]
        r = C.rel_log(self.EXP, caps)
        self.assertEqual((r["status"], r["j"]), (C.STOP, 3))
        self.assertEqual(C.rel_log(dict(self.EXP, **{"ambyte.3.log": 0}), [self.st(0, 1)])["status"], C.STOP)

    def test_from_console_capture(self):
        s = Sim()
        s.acct["evict"] = 0
        s.card["ambyte.log"] = bytearray(b"x" * 50)
        s.log_status()
        s.acct["evict"] = 1000
        s.log_status()
        s.line("ESP-ROM:esp32s3-20210327")
        s.line("rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)")
        s.acct["evict"] = 0
        s.log_status()
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "rel.log"
            p.write_bytes(bytes(s.out))
            caps = C.rel_captures_from_console([str(p)])
        self.assertEqual([c["kind"] for c in caps], ["status", "status", "boot", "status"])
        self.assertEqual(C.rel_log(self.EXP, caps)["j"], 1)


# =========================================================================== backoff

def trace_capture(entries: list[tuple], begin_end: tuple[int, int] | None = None) -> bytes:
    """Build a one-drain capture from (kind, us, *fields) tuples."""
    out = ["SLT_ON 2097152 1"]
    if begin_end:
        out.append(f"SDL_BEGIN {RUN} 100000 400 20 160 {begin_end[0]}")
    out.append(f"SLT_DRAIN 1 {len(entries)}")
    for i, (kind, us, *f) in enumerate(entries, 1):
        out.append(f"SLT_E {kind} {i} {us} " + " ".join(str(x) for x in f))
    out.append(f"SLT_HDR {len(entries)} 0 1 {len(entries)} 1000")
    if begin_end:
        out.append(f"SDL_END {RUN} 100399 {begin_end[1]}")
    return ("\r\n".join(out) + "\r\n").encode()


S = 1_000_000


class Backoff(unittest.TestCase):
    def test_sl_backoff_pass_and_fail(self):
        ok = trace_capture([("ROT", 10 * S, "fail", "rename", 5), ("ROT", 70 * S, "fail", "rename", 5),
                            ("ROT", 130 * S + 1, "ok", "-", 0)])
        r = C.backoff(C.parse_capture(ok))
        self.assertEqual(r["status"], C.PASS, r["reasons"])
        bad = trace_capture([("ROT", 10 * S, "fail", "rename", 5), ("ROT", 40 * S, "fail", "rename", 5)])
        self.assertEqual(C.backoff(C.parse_capture(bad))["status"], C.FAIL)
        # recorded gap a few ms short of 60 s is within the stamping tolerance; 1 s short is not
        near = trace_capture([("ROT", 10 * S, "fail", "rename", 5), ("ROT", 70 * S - 5000, "ok", "-", 0)])
        self.assertEqual(C.backoff(C.parse_capture(near))["status"], C.PASS)
        far = trace_capture([("ROT", 10 * S, "fail", "rename", 5), ("ROT", 69 * S, "ok", "-", 0)])
        self.assertEqual(C.backoff(C.parse_capture(far))["status"], C.FAIL)

    def l5(self, drop_at_ceiling=True, oks=1):
        ents = [("OPEN", 1 * S, "ambyte.log", 1_048_000, 0)]
        t = 10 * S
        for i in range(11):
            ents.append(("ROT", t, "fail", "rename", 5))
            ents.append(("OPEN", t + 1000, "ambyte.log", 1_048_000, 0))
            t += 60 * S + 1000
        size = 1_048_000 if not drop_at_ceiling else C.HARD_CAP - 100
        ents.append(("POP", t - 30 * S, 400))
        ents.append(("WR", t - 30 * S + 10, 400, 0 if not drop_at_ceiling else 0, size))
        ents.append(("POP", t - 29 * S, 400))
        ents.append(("DROP", t - 29 * S + 10, "rotate_blocked", 400))
        for k in range(oks):
            ents.append(("ROT", t + 5 * S + k * 70 * S, "ok", "-", 0))
        return trace_capture(ents)

    def test_l5_rules(self):
        r = C.backoff(C.parse_capture(self.l5()), row="L5")
        self.assertEqual(r["status"], C.PASS, r["reasons"])
        self.assertGreaterEqual(r["l5_blocked_s"], 600)
        self.assertEqual(C.backoff(C.parse_capture(self.l5(drop_at_ceiling=False)), row="L5")["status"], C.FAIL)
        self.assertEqual(C.backoff(C.parse_capture(self.l5(oks=2)), row="L5")["status"], C.FAIL)

    def test_l6_rules(self):
        good = [("ROT", 100 * S, "fail", "remove", 5), ("ROT", 161 * S, "fail", "remove", 5),
                ("ROT", 222 * S, "fail", "remove", 5), ("ROT", 283 * S, "ok", "-", 0)]
        r = C.backoff(C.parse_capture(trace_capture(good, (50 * S, 400 * S))), row="L6")
        self.assertEqual(r["status"], C.PASS, r["reasons"])
        two = good[:2] + good[3:]
        self.assertEqual(C.backoff(C.parse_capture(trace_capture(two, (50 * S, 400 * S))), row="L6")["status"], C.FAIL)
        early = good[:3] + [("ROT", 250 * S, "ok", "-", 0)]
        self.assertEqual(C.backoff(C.parse_capture(trace_capture(early, (50 * S, 400 * S))), row="L6")["status"],
                         C.FAIL)
        outside = C.backoff(C.parse_capture(trace_capture(good, (150 * S, 400 * S))), row="L6")
        self.assertEqual(outside["status"], C.FAIL)


# =========================================================================== L-8

class L8(unittest.TestCase):
    def build(self, half_ok=True, open_size_delta=0, append_torn=False):
        files = chain(5)
        s = Sim(files)
        s.drain()
        s.on()
        s.log_status()
        i0 = s.quiesce(inv=True, dumps=("ambyte.log",))
        s.drain()
        s.emit(10)
        s.drain()
        s.emit(10)
        s.drain()
        for _ in range(4):
            s.push()
        chunk = s.pop()
        half = len(chunk) // 2 + 3             # mid-record: the tail is torn
        before = s.size
        s.drain()                                # the synchronous final drain (after the witness line)
        drain_txt = s.out
        s.out = bytearray(drain_txt[:len(drain_txt)])
        wit_chunk = chunk if half_ok else chunk[:-1] + b"?"
        # the witness precedes its drain on the console
        tail = bytes(s.out)
        last_drain = tail.rindex(b"SLT_DRAIN")
        s.out = bytearray(tail[:last_drain])
        s.line(f"SLT_RESET_WITNESS /sdcard/logs/ambyte.log {before} {len(chunk)} {half} {sha(wit_chunk)} "
               f"{base64.b64encode(wit_chunk).decode()}")
        s.out += tail[last_drain:]
        s.line("HIL_FAULT fired kind=cpu_reset mode=reset_mid_write writer=sdlog op=write "
               "path=/sdcard/logs/ambyte.log errno=0 nth=9 slot=A sd_power=not_interrupted "
               "mechanism=esp_rom_software_reset_system")
        s.card["ambyte.log"] += chunk[:half]
        s.line("ESP-ROM:esp32s3-20210327")
        s.line("rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)")
        # post-boot: fresh trace from the auto-arm, fresh RAM ring
        s.seq, s.ents, s.ring, s.open = 1, [], [], False
        s.line("SLT_ON 2097152 1 autoarm")
        f = s.card["ambyte.log"]
        s.ent("OPEN", "ambyte.log", len(f) + open_size_delta, 1)
        s.open, s.committed = True, len(f)
        s.push(), s.push()
        c = s.pop()
        if append_torn:
            s.write(c)
            s.commit()
        s.close()
        old5 = s.card.pop("ambyte.5.log")
        for i in range(4, -1, -1):
            n = C.LOG_NAMES[i]
            if n in s.card:
                s.card[C.LOG_NAMES[i + 1]] = s.card.pop(n)
        s.ent("ROT", "ok", "-", 0)
        s.open_()
        if not append_torn:
            s.write(c)
            s.commit()
        s.drain()
        i1 = s.quiesce(inv=True, dumps=("ambyte.1.log", "ambyte.log"))
        s.drain()
        return s, i0, i1, files, old5

    def test_l8_pass(self):
        s, i0, i1, files, _ = self.build()
        blob = bytes(s.out)
        r = C.l8post(blob, C.parse_capture(blob), i0, i1, exports_of(files))
        self.assertEqual(r["status"], C.PASS, json.dumps(r["reasons"], indent=1))
        self.assertGreater(r["torn_tail_bytes"], 0)

    def test_l8_witness_mismatch_fails(self):
        s, i0, i1, files, _ = self.build(half_ok=False)
        blob = bytes(s.out)
        self.assertEqual(C.l8post(blob, C.parse_capture(blob), i0, i1, exports_of(files))["status"], C.FAIL)

    def test_l8_wrong_open_size_fails(self):
        s, i0, i1, files, _ = self.build(open_size_delta=1)
        blob = bytes(s.out)
        self.assertEqual(C.l8post(blob, C.parse_capture(blob), i0, i1, exports_of(files))["status"], C.FAIL)

    def test_l8_append_to_torn_file_fails(self):
        s, i0, i1, files, _ = self.build(append_torn=True)
        blob = bytes(s.out)
        self.assertNotEqual(C.l8post(blob, C.parse_capture(blob), i0, i1, exports_of(files))["status"], C.PASS)

    def test_l8_without_banner_voids(self):
        s, i0, i1, files, _ = self.build()
        blob = bytes(s.out).replace(b"ESP-ROM:", b"xxx-ROM ").replace(b"rst:0x", b"rsx:0x")
        self.assertEqual(C.l8post(blob, C.parse_capture(blob), i0, i1, exports_of(files))["status"], C.VOID)


if __name__ == "__main__":
    unittest.main()

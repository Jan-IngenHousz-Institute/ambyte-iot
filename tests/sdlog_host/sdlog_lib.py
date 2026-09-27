"""sd_logger host harness (tests/sdlog_host): builds the PRODUCTION
components/sd_logger/sd_logger.c - the working tree (head) and `git show <rev>:`
(base) - against the lock-step RTOS stand-in and the media shim, drives it with
scripted scenarios, and checks every run with an independent record oracle.

Oracle (contract C-10):
  - every captured log call has ONE expected record, computed here by a Python
    reference of the F-L0 framing rules (not by the firmware);
  - every retained log file must parse into complete '\\n'-framed lines, each
    byte-identical to exactly one expected record (or a pre-filled line); the
    only allowed exception is an unterminated EOF tail of a quarantined file;
  - byte reconciliation is exact (head): sum(expected) + prefill =
    intact + retained prefill + every accounting bucket + still-in-ring.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "tests" / "sdlog_host"
SHIM = ROOT / "tests" / "sd_shim"
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
FIXED_TIME = 1790000000
LINE_MAX = 256
FILE_CAP = 1024 * 1024
LOG_NAMES = ["ambyte.log"] + [f"ambyte.{i}.log" for i in range(1, 6)]


def cc() -> str:
    c = os.environ.get("CC") or shutil.which("clang") or "clang"
    if os.path.basename(c) == "cc":
        c = shutil.which("clang") or "clang"
    return c


def _run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"compile failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")


def build(out_dir: Path, rev: str | None, hil: bool = False) -> Path:
    """rev None = working tree (head); else git show <rev>: sources (base).
    hil=True (head only): the verification-build variant — sd_logger.c with its
    CONFIG_AMBYTE_EVQ_HIL trace hooks plus components/event_log/evq_hil_sdl.c
    (Sprint 2 H2/H8b), so the replay model is proven on byte-exact files."""
    out_dir.mkdir(parents=True, exist_ok=True)
    assert not (hil and rev is not None)
    name = ("sdlog_hil" if hil else "sdlog_head") if rev is None else f"sdlog_base_{rev}"
    src_dir = out_dir / (name + ".src")
    src_dir.mkdir(exist_ok=True)
    if rev is None:
        logger_c = ROOT / "components/sd_logger/sd_logger.c"
        inc_logger = ROOT / "components/sd_logger"
    else:
        for rel in ["components/sd_logger/sd_logger.c", "components/sd_logger/sd_logger.h"]:
            text = subprocess.run(["git", "show", f"{rev}:{rel}"], cwd=ROOT, capture_output=True, text=True,
                                  check=True).stdout
            (src_dir / Path(rel).name).write_text(text)
        logger_c = src_dir / "sd_logger.c"
        inc_logger = src_dir
    incs = [f"-I{SHIM / 'stubs'}", f"-I{SHIM}", f"-I{inc_logger}", f"-I{ROOT / 'components/sd_card'}",
            f"-I{ROOT / 'components/event_log/include'}"]
    defs = ['-DSD_MOUNT_POINT="./sdcard"'] + (["-DSDLOG_BASE"] if rev is not None else []) + \
        (["-DCONFIG_AMBYTE_EVQ_HIL=1", "-DEVQ_HIL_HOST"] if hil else [])
    warn = ["-Wall", "-Wextra"] + (["-Werror"] if rev is None else [])
    base = [cc(), "-std=gnu11", "-g", "-O1", *SAN, *warn, *incs, *defs]
    objdir = out_dir / (name + ".o")
    objdir.mkdir(exist_ok=True)
    objs = []
    o = objdir / "sd_logger.o"
    _run([*base, "-include", str(SHIM / "sd_shim.h"), "-c", str(logger_c), "-o", str(o)])
    objs.append(o)
    extra_srcs = [ROOT / "components/event_log/evq_hil_sdl.c"] if hil else []
    for s in [SHIM / "sd_shim.c", SHIM / "host_rtos.c", SHIM / "sd_host_stubs.c",
              ROOT / "components/sd_card/sd_diag_core.c", HOST / "sdlog_driver.c", *extra_srcs]:
        o = objdir / (s.stem + ".o")
        extra = ["-Wno-error"] if s.name == "sdlog_driver.c" else []
        _run([*base, *extra, "-c", str(s), "-o", str(o)])
        objs.append(o)
    exe = out_dir / name
    _run([cc(), *SAN, *map(str, objs), "-lpthread", "-o", str(exe)])
    return exe


# ── reference framing (independent of the firmware) ─────────────────────────

PREFIX = time.strftime("%Y-%m-%d %H:%M:%S  ", time.gmtime(FIXED_TIME)).encode()


def _level_kept(fmt: bytes) -> bool:
    p = 0
    if fmt[:1] == b"\x1b":
        p = 1
        if fmt[p:p + 1] == b"[":
            p += 1
            while p < len(fmt) and not (0x40 <= fmt[p] <= 0x7E):
                p += 1
            if p < len(fmt):
                p += 1
    return fmt[p:p + 1] in (b"E", b"W")


def expected_record(fmt: bytes, text: bytes) -> bytes | None:
    """The one record a log call must produce (F-L0), or None if not captured."""
    if not _level_kept(fmt):
        return None
    buf = LINE_MAX + 1                                    # formatting buffer incl. NUL
    room = buf - len(PREFIX)                              # vsnprintf capacity after the prefix
    trunc = len(text) >= room
    kept = text[: room - 1] if trunc else text
    raw = PREFIX + kept
    # ANSI CSI strip on the kept bytes, same scan the sink performs
    out = bytearray(PREFIX)
    r = len(PREFIX)
    while r < len(raw):
        c = raw[r]
        if c == 0x1B:
            r += 1
            if r < len(raw) and raw[r] == 0x5B:
                r += 1
                while r < len(raw) and not (0x40 <= raw[r] <= 0x7E):
                    r += 1
            r += 1
            continue
        out.append(c)
        r += 1
    while len(out) > len(PREFIX) and out[-1] in (0x0A, 0x0D):
        out.pop()
    for i in range(len(PREFIX), len(out)):
        if out[i] in (0x0A, 0x0D):
            out[i] = 0x20
    if len(out) > LINE_MAX - 1:
        del out[LINE_MAX - 1:]
        trunc = True
    if trunc:
        del out[LINE_MAX - 3:]
        out += b"~T"
    out += b"\n"
    return bytes(out)


# ── device ──────────────────────────────────────────────────────────────────

@dataclass
class Dev:
    exe: Path
    state: Path
    variant: str
    proc: subprocess.Popen | None = None
    calls: list = field(default_factory=list)       # (fmt, text)
    out: list = field(default_factory=list)
    prefill: int = 0
    trace: list = field(default_factory=list)       # non-JSON lines (SLT_*): hil variant only

    def start(self) -> None:
        env = dict(os.environ, TZ="UTC", ASAN_OPTIONS="detect_leaks=0")
        self.proc = subprocess.Popen([str(self.exe), str(self.state)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, env=env)

    def _json(self) -> dict:
        """Next JSON reply; SLT_* trace lines (which the hil variant may print at
        any time, e.g. the watermark) are stashed in self.trace, never parsed."""
        assert self.proc and self.proc.stdout
        while True:
            ln = self.proc.stdout.readline()
            if not ln:
                raise RuntimeError("driver exited")
            if ln.startswith("{"):
                return json.loads(ln)
            self.trace.append(ln.rstrip("\n"))

    def cmd(self, line: str, expect: bool = False) -> dict | None:
        assert self.proc and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        if line.startswith(("log ", "logf ")):
            o = self._json()
            self.calls.append((bytes.fromhex(o["fmt_hex"]), bytes.fromhex(o["text_hex"])))
            return o
        if expect:
            o = self._json()
            if "error" in o:
                raise RuntimeError(o["error"])
            self.out.append(o)
            return o
        return None

    def init(self) -> None:
        self.cmd("init", expect=True)

    def log(self, seq: int, textlen: int = 60) -> None:
        self.cmd(f"log {seq} {textlen}")

    def logf(self, fmt: bytes, arg: bytes) -> None:
        self.cmd(f"logf {fmt.hex()} {arg.hex() if arg else '-'}")

    def step(self, n: int = 1) -> None:
        self.cmd(f"step {n}")

    def arm(self, op: str, mode: str, nth: int = 1, count: int = 1, sub: str | None = None) -> None:
        o = self.cmd(f"arm {op} {mode} {nth} {count}" + (f" {sub}" if sub else ""), expect=True)
        assert o["arm"] == "ok", o

    def acct(self) -> dict:
        return self.cmd("acct", expect=True)

    def opcount(self) -> dict:
        return self.cmd("opcount", expect=True)

    def finish(self) -> None:
        if self.proc:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=60)

    def prefill_file(self, name: str, nbytes: int) -> None:
        d = self.state / "sdcard" / "logs"
        d.mkdir(parents=True, exist_ok=True)
        line = b"PREFILL|" + b"p" * 90 + b"\n"
        data = line * (nbytes // len(line))
        with open(d / name, "ab") as f:
            f.write(data)
        self.prefill += len(data)

    def ops(self) -> list[dict]:
        p = self.state / "ops.jsonl"
        return [json.loads(x) for x in p.read_text().splitlines()] if p.exists() else []

    def console(self) -> str:
        p = self.state / "console.log"
        return p.read_text(errors="replace") if p.exists() else ""


def new_dev(exe: Path, state: Path, variant: str) -> Dev:
    if state.exists():
        shutil.rmtree(state)
    state.mkdir(parents=True)
    return Dev(exe=exe, state=state, variant=variant)


# ── oracle ──────────────────────────────────────────────────────────────────

PREFILL_RE = re.compile(rb"^PREFILL\|p+\n$")


def oracle(dev: Dev, acct: dict) -> dict:
    """Returns a report; report['ok'] is the C-10 verdict for this run."""
    expected: dict[bytes, int] = {}
    total = 0
    captured = 0
    for fmt, text in dev.calls:
        rec = expected_record(fmt, text)
        if rec is None:
            continue
        captured += 1
        expected[rec] = expected.get(rec, 0) + 1
        total += len(rec)
    logs = dev.state / "sdcard" / "logs"
    intact = prefill_found = 0
    bad_lines: list[str] = []
    tails: list[tuple[str, int]] = []
    remaining = dict(expected)
    for name in LOG_NAMES:
        p = logs / name
        if not p.exists():
            continue
        data = p.read_bytes()
        pos = 0
        while pos < len(data):
            nl = data.find(b"\n", pos)
            if nl < 0:
                tails.append((name, len(data) - pos))
                break
            line = data[pos:nl + 1]
            pos = nl + 1
            if PREFILL_RE.match(line):
                prefill_found += len(line)
            elif remaining.get(line, 0) > 0:
                remaining[line] -= 1
                intact += len(line)
            else:
                bad_lines.append(f"{name}: {line[:80]!r}")
    rep = {"captured": captured, "expected_bytes": total, "intact": intact, "prefill": dev.prefill,
           "prefill_found": prefill_found, "bad_lines": bad_lines[:20], "n_bad": len(bad_lines),
           "tails": tails, "missing_records": sum(remaining.values())}
    a = acct.get("acct")
    if a is None:        # base: no accounting exists
        rep["reconcile"] = None
        rep["ok"] = not bad_lines and not tails
        return rep
    buckets = (a["dropped_ring_bytes"] + a["dropped_rotate_blocked_bytes"] + a["dropped_unavailable_bytes"] +
               a["rolled_back_bytes"] + a["indeterminate_bytes"] + a["lost_unwritten_bytes"] +
               a["retention_evicted_bytes"] + a["buffered_bytes"])
    lhs = total + dev.prefill
    rhs = intact + prefill_found + buckets
    tail_total = sum(t for _, t in tails)
    rep["reconcile"] = {"lhs": lhs, "rhs": rhs, "D": rhs - lhs - 0, "buckets": buckets, "tail_total": tail_total}
    # Exact: a torn tail's bytes are not intact; they are part of the
    # indeterminate bucket (the only bucket whose bytes may be physically present).
    ok = not bad_lines and lhs == rhs and tail_total <= a["indeterminate_bytes"]
    if tails and not a["indeterminate_bytes"]:
        ok = False
    rep["ok"] = ok
    return rep


def refs_ok(acct: dict) -> bool:
    return acct["violations"] == 0 and acct["refs_at_delay"] == 0 and acct["refs"] == 0


def sd_logger_console_lines(dev: Dev) -> int:
    return sum(1 for ln in dev.console().splitlines() if " sd_logger: " in ln)

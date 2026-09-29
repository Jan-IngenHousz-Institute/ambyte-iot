"""Sprint 2 hardware-run helper (contract a722b3f2 §6, §9): drives the running
single-owner serial daemon (tools/evq_hil/serial_daemon.py) through its FIFO and
slices its capture by byte offset, so every row's evidence is a bound byte range
of one private log.

    python s2run.py --phase P2 off                            # current capture offset
    python s2run.py --phase P2 cmd "evq_hil state" --until 'HIL_STATE' --timeout 30
    python s2run.py --phase P2 drainloop --every 20 --for 900  # H2 rolling drains (+ on SLT_WM)
    python s2run.py --phase P2 slice A B --out $PRIV/p2/rowX.log
    python s2run.py --phase P2 record NAME --outputs a,b -- <host command ...>

The private directory is ~/.local/share/ambyte-write-integrity/sprint-02 (0700).
Only this tool, the daemon and esptool (with the daemon stopped) touch the port.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PRIV = Path.home() / ".local/share/ambyte-write-integrity/sprint-02"
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def paths(phase: str) -> tuple[Path, Path]:
    return PRIV / f"{phase}.log", PRIV / f"{phase}.ctl"


def size(p: Path) -> int:
    return p.stat().st_size if p.exists() else 0


def send(ctl: Path, *cmds: str, gap: float = 0.4) -> None:
    for c in cmds:
        with open(ctl, "w") as f:           # FIFO: one line per open (daemon reopens)
            f.write(c + "\n")
        time.sleep(gap)


def wait(log: Path, regex: str, start: int, timeout: float) -> int:
    rx = re.compile(regex.encode())
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        with open(log, "rb") as f:
            f.seek(start)
            m = rx.search(f.read())
        if m:
            return start + m.end()
        time.sleep(0.5)
    return -1


def slice_bytes(log: Path, a: int, b: int | None) -> bytes:
    with open(log, "rb") as f:
        f.seek(a)
        return f.read() if b is None else f.read(b - a)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", required=True)
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("off")
    c = sub.add_parser("cmd")
    c.add_argument("line")
    c.add_argument("--until", default=r"ambyte> ")
    c.add_argument("--timeout", type=float, default=60)
    c.add_argument("--quiet", action="store_true")
    d = sub.add_parser("drainloop")
    d.add_argument("--every", type=float, default=20)
    d.add_argument("--for", dest="dur", type=float, required=True)
    d.add_argument("--stop-file")
    s = sub.add_parser("slice")
    s.add_argument("a", type=int)
    s.add_argument("b", type=int, nargs="?")
    s.add_argument("--out", required=True)
    r = sub.add_parser("record")
    r.add_argument("name")
    r.add_argument("--outputs", default="")
    r.add_argument("rest", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    log, ctl = paths(a.phase)
    os.umask(0o077)

    if a.mode == "off":
        print(size(log))
        return 0
    if a.mode == "cmd":
        start = size(log)
        send(ctl, a.line)
        end = wait(log, a.until, start, a.timeout)
        out = slice_bytes(log, start, end if end > 0 else None)
        if not a.quiet:
            sys.stdout.write(out.decode(errors="replace"))
        print(f"\n#S2RUN range {start} {end if end > 0 else size(log)} {'ok' if end > 0 else 'TIMEOUT'}", file=sys.stderr)
        return 0 if end > 0 else 3
    if a.mode == "drainloop":
        # H2: drain every `every` s, and at once when the device announces SLT_WM.
        t_end = time.monotonic() + a.dur
        last = 0.0
        seen = size(log)
        while time.monotonic() < t_end:
            if a.stop_file and Path(a.stop_file).exists():
                break
            now = time.monotonic()
            with open(log, "rb") as f:
                f.seek(seen)
                chunk = f.read()
            wm = b"SLT_WM " in chunk
            seen += len(chunk)
            if wm or now - last >= a.every:
                send(ctl, "evq_hil sdlog_trace drain", gap=0.1)
                last = now
            time.sleep(0.5)
        send(ctl, "evq_hil sdlog_trace drain", gap=0.1)
        return 0
    if a.mode == "slice":
        data = slice_bytes(log, a.a, a.b)
        Path(a.out).write_bytes(data)
        return 0
    if a.mode == "record":
        cmd = a.rest[1:] if a.rest and a.rest[0] == "--" else a.rest
        t0 = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        rc = subprocess.run(cmd).returncode
        t1 = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        rec = [sys.executable, str(ROOT / "tools/check_evidence_manifest.py"), "--record",
               "--manifest", str(PRIV / "hw-manifest.jsonl"), "--name", a.name, "--start", t0, "--end", t1,
               "--exit", str(rc), "--outputs", a.outputs]
        subprocess.run(rec, check=False)
        return rc
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

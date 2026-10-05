"""Phase driver for the HIL run (contract §4): talks to the running
serial_daemon through its FIFO and slices the capture by byte offset.

    python hilrun.py p4 --run hil-20260926-P4-01 [--n 3200 --pad 4000 --hz 20]
    python hilrun.py inv --tag after-P4          # quiescent inventory set
    python hilrun.py cmd "evq_hil state" ...     # send and print the reply slice
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

P = Path.home() / ".local/share/ambyte-evq-hil"
LOG = P / "cap-HIL.log"
CTL = P / "d.ctl"


def off() -> int:
    return LOG.stat().st_size


def send(*cmds: str, gap: float = 0.4) -> None:
    # One FIFO open per command: the daemon reopens its read end after each
    # writer closes, so a held-open writer can hit EPIPE in between.
    for c in cmds:
        for attempt in range(20):
            try:
                with open(CTL, "w") as f:
                    f.write(c + "\n")
                break
            except BrokenPipeError:
                time.sleep(0.2)
        time.sleep(gap)


def wait(regex: str, start: int, timeout: float = 600, count: int = 1) -> int:
    rx = re.compile(regex.encode())
    end = time.time() + timeout
    while time.time() < end:
        data = LOG.read_bytes()[start:]
        ms = list(rx.finditer(data))
        if len(ms) >= count:
            return start + ms[count - 1].end()
        time.sleep(0.5)
    raise TimeoutError(f"no {regex!r} x{count} within {timeout}s")


def slice_text(a: int, b: int | None = None) -> str:
    return LOG.read_bytes()[a:b].decode(errors="replace")


def cmd_reply(cmd: str, done_regex: str, timeout: float = 120) -> str:
    a = off()
    send(cmd)
    b = wait(done_regex, a, timeout)
    return slice_text(a, b)


def quiescent_inventory(tag: str, sd_dirs=("/sdcard/events", "/sdcard/evq"), full: bool = False) -> tuple[int, int]:
    """keeper pause -> flash_inv -> sd_inv each record dir -> index -> cursor
    -> claims -> io drain -> evlog -> keeper run. One command at a time, each
    awaited, so no two dumps interleave."""
    a = off()
    print(f"[{tag}] inventory from offset {a}", flush=True)
    cmd_reply("evq_hil keeper pause", r"HIL_OK keeper_paused=1")
    time.sleep(2)                      # let an in-flight keeper pass finish
    cmd_reply("evq_hil flash_inv" + (" full" if full else ""), r"EVQ_FEND \d+ \d+", 1800)
    for d in sd_dirs:
        cmd_reply(f"evq_hil sd_inv {d} " + ("full" if full else "lines"), r"EVQ_SDEND \d+ \d+|EVQ_FERR", 3600)
    cmd_reply("evq_hil index", r"EVQ_IXEND")
    cmd_reply("evq_hil cursor", r"EVQ_CUR [^\n]*\n")
    cmd_reply("evq_hil claims", r"EVQ_CLAIMS [^\n]*\n")
    cmd_reply("evq_hil io drain", r"EVQ_TR_HDR [^\n]*\n", 300)
    cmd_reply("evq_hil stacks", r"EVQ_STACKEND[^\n]*\n")
    cmd_reply("evlog", r"sd_retired_names=\d+ sd_bad_copies=\d+")
    cmd_reply("evq_hil state", r"EVQ_STATE [^\n]*\n")
    cmd_reply("evq_hil keeper run", r"HIL_OK keeper_paused=0")
    b = off()
    (P / f"inv-{tag}.range").write_text(f"{a} {b}\n")
    print(f"[{tag}] inventory done {a}..{b}", flush=True)
    return a, b


def p4(run: str, n: int, pad: int, hz: int, k0: int = 0) -> None:
    a = off()
    (P / f"fill-{run}.start").write_text(str(a))
    send(f"evq_hil fill {run} {k0} {n} {pad} {hz}")
    wait(r"EVQ_BEGIN " + re.escape(run), a, 60)
    done_rx = re.compile((r"EVQ_END " + re.escape(run)).encode())
    last = a
    tick = 0
    while not done_rx.search(LOG.read_bytes()[a:]):
        time.sleep(5)
        tick += 1
        send("evq_hil io drain")                   # trace drained well inside the ring
        if tick % 2 == 0:
            send("evlog", "status")
        cur = off()
        acc = LOG.read_bytes()[a:cur].count(b"EVQ_ACC " + run.encode() + b" ")
        ref = LOG.read_bytes()[a:cur].count(b"EVQ_REF " + run.encode() + b" ")
        print(f"  {run}: acc={acc} ref={ref}", flush=True)
        if ref:
            print("  REFUSAL seen -> stop condition check", flush=True)
        last = cur
    time.sleep(3)
    send("evq_hil io drain", "evlog", "status")
    time.sleep(4)
    print(slice_text(last)[-1500:])


def main() -> int:
    global LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=str(LOG))
    sub = ap.add_subparsers(dest="cmd", required=True)
    x = sub.add_parser("p4")
    x.add_argument("--run", required=True)
    x.add_argument("--n", type=int, default=3200)
    x.add_argument("--pad", type=int, default=4000)
    x.add_argument("--hz", type=int, default=20)
    x.add_argument("--k0", type=int, default=0)
    i = sub.add_parser("inv")
    i.add_argument("--tag", required=True)
    i.add_argument("--full", action="store_true")
    i.add_argument("--dirs", nargs="*", default=["/sdcard/events", "/sdcard/evq"])
    c = sub.add_parser("cmd")
    c.add_argument("cmds", nargs="+")
    c.add_argument("--wait", type=float, default=4)
    a = ap.parse_args()
    LOG = Path(a.log)
    if a.cmd == "p4":
        p4(a.run, a.n, a.pad, a.hz, a.k0)
    elif a.cmd == "inv":
        quiescent_inventory(a.tag, tuple(a.dirs), a.full)
    else:
        s = off()
        send(*a.cmds)
        time.sleep(a.wait)
        sys.stdout.write(slice_text(s))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

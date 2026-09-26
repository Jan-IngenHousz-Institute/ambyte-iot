"""Persistent single-owner serial session for the HIL phases.

Opening the ESP32-S3 USB-Serial-JTAG port can reset the CPU (modem-control
lines), so one daemon holds the port for a whole phase: every received byte is
appended to LOG (with a host-time index LOG.idx: "<unix_ms> <byte_offset>"
per chunk, and host notes - opens, sends, resets - in LOG.notes), commands
are read from the FIFO CTL and written with CRLF.
After an injected reset the USB device may re-enumerate: the daemon reopens
with the same line states and keeps logging. `esptool` needs the port: stop
the daemon first (send "__quit__").

    python serial_daemon.py --log LOG --ctl CTL [--dtr 1 --rts 1]
    python serial_daemon.py send --ctl CTL "evq_hil state" ...
    python serial_daemon.py wait --log LOG --regex 'EVQ_FEND' --timeout 600 [--from-offset N]
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import threading
import time
from pathlib import Path

PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:37:2F:FF:E7:04-if00"


def run(log: Path, ctl: Path, dtr: bool, rts: bool) -> int:
    import serial

    if not ctl.exists():
        os.mkfifo(ctl, 0o600)
    stop = threading.Event()
    lock = threading.Lock()
    state = {"conn": None, "opens": 0}
    lf = open(log, "ab", buffering=0)
    idx = open(str(log) + ".idx", "a", buffering=1)
    notes = open(str(log) + ".notes", "a", buffering=1)

    def note(msg: str) -> None:
        # Kept OUT of the device byte stream (a note spliced mid-line would
        # corrupt an inventory line): "<unix_ms> <log byte offset> <msg>".
        with lock:
            notes.write(f"{int(time.time() * 1000)} {lf.tell()} {msg}\n")

    def opener():
        while not stop.is_set():
            try:
                c = serial.Serial(port=None, baudrate=115200, timeout=0.1, exclusive=True)
                c.dtr = dtr
                c.rts = rts
                c.port = PORT
                c.open()
                state["opens"] += 1
                note(f"port open #{state['opens']} dtr={int(dtr)} rts={int(rts)}")
                return c
            except Exception as e:  # noqa: BLE001
                time.sleep(0.3)
                last = str(e)
        return None

    def reader():
        while not stop.is_set():
            c = state["conn"]
            if c is None:
                c = opener()
                state["conn"] = c
                if c is None:
                    return
            try:
                data = c.read(65536)
            except Exception as e:  # noqa: BLE001
                note(f"port lost: {e}")
                try:
                    c.close()
                except Exception:  # noqa: BLE001
                    pass
                state["conn"] = None
                time.sleep(0.3)
                continue
            if data:
                with lock:
                    off = lf.tell() if hasattr(lf, "tell") else 0
                    lf.write(data)
                idx.write(f"{int(time.time() * 1000)} {off}\n")

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        while not stop.is_set():
            with open(ctl, "r") as f:            # blocks until a writer opens
                for line in f:
                    cmd = line.rstrip("\n")
                    if cmd == "__quit__":
                        stop.set()
                        break
                    if cmd == "__hardreset__":
                        # esptool's USB-Serial-JTAG hard reset: RTS asserted with
                        # DTR released resets the chip; then back to the idle
                        # (dtr, rts) state, which neither resets nor straps boot.
                        c = state["conn"]
                        if c is not None:
                            note("hard reset (RTS pulse)")
                            try:
                                c.dtr = False
                                c.rts = True
                                time.sleep(0.1)
                                c.rts = False
                                time.sleep(0.05)
                                c.dtr = dtr
                                c.rts = rts
                            except Exception as e:  # noqa: BLE001
                                note(f"hard reset: {e}")
                        continue
                    for _ in range(50):
                        c = state["conn"]
                        if c is not None:
                            break
                        time.sleep(0.1)
                    if c is None:
                        note(f"DROPPED (no port): {cmd}")
                        continue
                    note(f"send: {cmd}")
                    try:
                        c.write((cmd + "\r\n").encode())
                        c.flush()
                    except Exception as e:  # noqa: BLE001
                        note(f"send failed: {e}")
                    time.sleep(0.05)
    finally:
        stop.set()
        t.join(timeout=2)
        c = state["conn"]
        if c is not None:
            c.close()
        note("daemon exit")
        lf.close()
        idx.close()
        notes.close()
    return 0


def send(ctl: Path, cmds: list[str], gap: float) -> int:
    with open(ctl, "w") as f:
        for c in cmds:
            f.write(c + "\n")
            f.flush()
            time.sleep(gap)
    return 0


def wait(log: Path, regex: str, timeout: float, from_offset: int) -> int:
    rx = re.compile(regex.encode())
    end = time.time() + timeout
    while time.time() < end:
        data = log.read_bytes()[from_offset:]
        m = rx.search(data)
        if m:
            print(f"matched at {from_offset + m.start()}: {data[m.start():m.start() + 200]!r}")
            return 0
        time.sleep(1)
    print("timeout")
    return 1


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "send":
        ap = argparse.ArgumentParser()
        ap.add_argument("mode")
        ap.add_argument("--ctl", required=True)
        ap.add_argument("--gap", type=float, default=0.3)
        ap.add_argument("cmds", nargs="+")
        a = ap.parse_args()
        return send(Path(a.ctl), a.cmds, a.gap)
    if len(sys.argv) > 1 and sys.argv[1] == "wait":
        ap = argparse.ArgumentParser()
        ap.add_argument("mode")
        ap.add_argument("--log", required=True)
        ap.add_argument("--regex", required=True)
        ap.add_argument("--timeout", type=float, default=600)
        ap.add_argument("--from-offset", type=int, default=0)
        a = ap.parse_args()
        return wait(Path(a.log), a.regex, a.timeout, a.from_offset)
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--ctl", required=True)
    ap.add_argument("--dtr", type=int, default=1)
    ap.add_argument("--rts", type=int, default=1)
    a = ap.parse_args()
    return run(Path(a.log), Path(a.ctl), bool(a.dtr), bool(a.rts))


if __name__ == "__main__":
    raise SystemExit(main())

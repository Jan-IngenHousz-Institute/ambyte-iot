"""Single-owner serial console for the Ambyte evq HIL runs.

Sends CLI commands and captures everything the device prints to a log file.
Opening the USB-JTAG port can reset the board; DTR/RTS are held low before
open to avoid the explicit reset lines, but a boot may still be captured.

Usage:
  python console.py --log FILE [--settle S] [--wait S] "cmd" ["cmd" ...]
  python console.py --log FILE --until REGEX --timeout S "cmd"
"""
import argparse
import re
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402  (A2: explicit bench identity, no default board)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--port", default=None)
    ap.add_argument("--settle", type=float, default=2.0, help="read time after open, before the first command")
    ap.add_argument("--wait", type=float, default=3.0, help="read time after each command")
    ap.add_argument("--until", help="after the last command, read until this regex matches")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()
    if a.port is None:
        a.port = bench.port()
    out = bytearray()
    with serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True) as conn:
        conn.dtr = False
        conn.rts = False
        conn.port = a.port
        conn.open()

        def pump(seconds: float) -> None:
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                out.extend(conn.read(65536))

        pump(a.settle)
        for c in a.cmds:
            conn.write((c + "\r\n").encode())
            pump(a.wait)
        if a.until:
            rx = re.compile(a.until.encode())
            end = time.monotonic() + a.timeout
            while time.monotonic() < end and not rx.search(out):
                out.extend(conn.read(65536))
    Path(a.log).write_bytes(bytes(out))
    sys.stdout.write(out.decode(errors="replace"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

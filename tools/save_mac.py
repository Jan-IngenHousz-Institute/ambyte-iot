#!/usr/bin/env python3
"""Read a board's base MAC from efuse (via the bootloader) and append it to a
CSV log on this computer.

Unlike get_mac.py (which parses the running firmware's `status` output), this
talks to the ROM / second-stage bootloader over the USB-C port using esptool —
the *same* channel `pio run -t upload` uses to flash. So it reads the MAC
straight from efuse, which means:
  * it's fast and deterministic (no waiting for the firmware to boot), and
  * it works even on a blank or bricked unit that never comes up.
On the ESP32-S3 the base MAC equals the Wi-Fi STA MAC the firmware reports, so
the value matches get_mac.py.

Run it right before/after flashing a board:

    python tools/save_mac.py                  # auto-detect port, append a row
    python tools/save_mac.py --port COM7
    python tools/save_mac.py --note "field unit 12"

Each run appends one row to flashed_macs.csv in the repo root (gitignored):

    timestamp_utc,mac,port,git_commit,note
    2026-07-07T12:34:56Z,60:55:F9:AA:BB:CC,COM7,09782fa,field unit 12

esptool is installed in this repo's venv (`uv pip install esptool`); if it isn't
importable this script falls back to the copy bundled with the PlatformIO
espressif32 platform.

Note: reading the MAC resets the chip into download mode and back (esptool's
default), so briefly interrupts a running board — expected when flashing.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# Reuse the port auto-detect + MAC regex from the console reader (same tools dir,
# which is on sys.path[0] when this file is run as a script).
try:
    from get_mac import _autodetect_port, _MAC_RE
except ImportError:  # pragma: no cover - fallback if run oddly
    _autodetect_port = None
    _MAC_RE = re.compile(r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

PROJECT_DIR = Path(__file__).resolve().parent.parent
DEFAULT_CSV = PROJECT_DIR / "flashed_macs.csv"
CSV_HEADER = ["timestamp_utc", "mac", "port", "git_commit", "note"]


def _esptool_cmd():
    """Return the argv prefix that invokes esptool, or None if unavailable."""
    try:
        import esptool  # noqa: F401 — presence check only
        return [sys.executable, "-m", "esptool"]
    except ImportError:
        pass
    # Fall back to the copy bundled with the PlatformIO espressif32 platform.
    bundled = Path.home() / ".platformio" / "packages" / "tool-esptoolpy"
    if (bundled / "esptool.py").is_file():
        return [sys.executable, str(bundled / "esptool.py")]
    return None


def _git_commit():
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=str(PROJECT_DIR), capture_output=True, text=True, timeout=5)
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


def read_mac_via_esptool(port, verbose=False):
    """Run `esptool read-mac` and return the MAC string, or raise SystemExit."""
    cmd = _esptool_cmd()
    if cmd is None:
        raise SystemExit(
            "save_mac: esptool not found — `uv pip install esptool` "
            "(or run inside the repo venv).")

    # Use the underscore form: esptool v4 requires "read_mac", and v5 still
    # accepts it (with a deprecation warning) — so one spelling works on both.
    cmd = cmd + ["--chip", "esp32s3", "--port", port, "read_mac"]
    if verbose:
        sys.stderr.write("save_mac: " + " ".join(cmd) + "\n")

    try:
        # NO_COLOR keeps esptool v5's rich output plain so parsing is reliable.
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=60,
            env={**os.environ, "NO_COLOR": "1"})
    except FileNotFoundError as exc:
        raise SystemExit(f"save_mac: cannot launch esptool: {exc}")
    except subprocess.TimeoutExpired:
        raise SystemExit(f"save_mac: esptool timed out talking to {port}.")

    output = _ANSI_RE.sub("", (proc.stdout or "") + "\n" + (proc.stderr or ""))
    if verbose:
        sys.stderr.write(output)

    m = _MAC_RE.search(output)
    if not m:
        # Surface esptool's own diagnostics so the failure is obvious.
        if not verbose:
            sys.stderr.write(output)
        raise SystemExit(
            f"save_mac: esptool did not report a MAC on {port} "
            f"(exit {proc.returncode}). Is the board connected by USB-C, and is "
            "the port free (close any serial monitor)?")
    return m.group(1).upper()


def append_row(csv_path, mac, port, note):
    csv_path = Path(csv_path)
    new_file = not csv_path.exists()
    row = {
        "timestamp_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "mac": mac,
        "port": port,
        "git_commit": _git_commit(),
        "note": note or "",
    }
    with csv_path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_HEADER)
        if new_file:
            writer.writeheader()
        writer.writerow(row)
    return row


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Read a board's base MAC via esptool and log it to a CSV.")
    parser.add_argument("--port", help="Serial port (e.g. COM7 or /dev/ttyACM0). "
                                       "Auto-detected if omitted.")
    parser.add_argument("--out", default=str(DEFAULT_CSV),
                        help=f"CSV file to append to (default {DEFAULT_CSV.name} "
                             "in the repo root).")
    parser.add_argument("--note", default="", help="Free-text note stored with the row.")
    parser.add_argument("--verbose", action="store_true",
                        help="Echo esptool's output to stderr.")
    args = parser.parse_args(argv)

    if args.port:
        port = args.port
    elif _autodetect_port is not None:
        port = _autodetect_port()
    else:
        raise SystemExit("save_mac: --port is required (get_mac auto-detect unavailable).")

    if args.verbose:
        sys.stderr.write(f"save_mac: reading MAC via esptool on {port}\n")

    mac = read_mac_via_esptool(port, verbose=args.verbose)
    row = append_row(args.out, mac, port, args.note)

    sys.stderr.write(
        f"save_mac: logged {mac} to {args.out} "
        f"({row['timestamp_utc']}, commit {row['git_commit'] or 'n/a'})\n")
    print(mac)  # stdout = just the MAC, for scripting
    return 0


if __name__ == "__main__":
    sys.exit(main())

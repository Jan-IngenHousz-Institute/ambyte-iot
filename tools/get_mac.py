#!/usr/bin/env python3
"""Read an ambyte board's Wi-Fi STA MAC over the USB-C console.

The firmware exposes an esp_console REPL (prompt `ambyte> `) on the ESP32-S3's
native USB-Serial-JTAG port (the USB-C connector). Its `status` command already
prints the board's STA MAC as ` - MAC: AA:BB:CC:DD:EE:FF`, so this script just
drives that command and parses the line back out — no firmware change or reflash
needed.

Usage:
    python tools/get_mac.py                 # auto-detect the port, print the MAC
    python tools/get_mac.py --port COM7     # or an explicit port / /dev/ttyACM0
    python tools/get_mac.py --verbose       # also echo the raw console output

On success it prints just the MAC (e.g. `AA:BB:CC:DD:EE:FF`) to stdout and exits
0, so it's easy to capture in a shell:  MAC=$(python tools/get_mac.py)

Requires pyserial (ships with the PlatformIO core; otherwise `pip install pyserial`).

Notes:
  * Opening the USB-Serial-JTAG port does NOT reset the chip, so this is safe to
    run against a live board; the CLI answers within a second or two.
  * If the board is busy (e.g. mid-measurement) the console may lag — the script
    retries within a short window before giving up.
"""

import argparse
import re
import sys
import time

# ESP32-S3 native USB-Serial-JTAG identifiers (the USB-C console port).
_ESP_USB_VID = 0x303A
_ESP_USB_JTAG_PID = 0x1001

_DEFAULT_BAUD = 115200
_TOTAL_WINDOW_S = 15.0    # give up after this if the CLI never answers
_ATTEMPT_GAP_S = 1.5      # between `status` send attempts
_REPLY_WAIT_S = 2.5       # read for the reply after each send

# " - MAC: AA:BB:CC:DD:EE:FF" (also tolerates the raw 12-hex form, just in case).
_MAC_RE = re.compile(
    r"MAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}|[0-9A-Fa-f]{12})"
)


def _autodetect_port():
    """Return the single ESP32-S3 USB-Serial-JTAG port, or None.

    Raises SystemExit with guidance if zero or several candidates are found so
    the caller knows to pass --port explicitly.
    """
    from serial.tools import list_ports

    candidates = [
        p for p in list_ports.comports()
        if p.vid == _ESP_USB_VID and p.pid == _ESP_USB_JTAG_PID
    ]
    if not candidates:
        # Fall back to any Espressif VID (e.g. a UART bridge) as a hint only.
        others = [p for p in list_ports.comports() if p.vid == _ESP_USB_VID]
        msg = "get_mac: no ESP32-S3 USB-Serial-JTAG port found."
        if others:
            hint = ", ".join(f"{p.device} ({p.description})" for p in others)
            msg += (" Espressif devices seen on other interfaces: " + hint +
                    " — connect the USB-C (native USB) port, or pass --port.")
        else:
            msg += " Is the board connected by USB-C and powered?"
        raise SystemExit(msg)
    if len(candidates) > 1:
        listing = ", ".join(f"{p.device}" for p in candidates)
        raise SystemExit(
            f"get_mac: multiple boards found ({listing}). "
            "Pass --port to pick one.")
    return candidates[0].device


def read_mac(port, baud, verbose=False):
    """Drive the CLI `status` command and return the MAC string, or None."""
    import serial

    try:
        ser = serial.Serial(port, baud, timeout=0.3)
    except Exception as exc:
        raise SystemExit(
            f"get_mac: cannot open {port}: {exc} "
            "(close any open serial monitor and retry).")

    try:
        deadline = time.time() + _TOTAL_WINDOW_S
        while time.time() < deadline:
            try:
                ser.reset_input_buffer()
                # Leading newline clears any half-typed line at the prompt.
                ser.write(b"\r\nstatus\r\n")
                ser.flush()
            except Exception as exc:
                raise SystemExit(f"get_mac: serial write failed: {exc}")

            reply = b""
            reply_deadline = time.time() + _REPLY_WAIT_S
            while time.time() < reply_deadline:
                chunk = ser.read(512)
                if not chunk:
                    continue
                reply += chunk
                m = _MAC_RE.search(reply.decode(errors="replace"))
                if m:
                    if verbose:
                        sys.stderr.write(reply.decode(errors="replace"))
                        sys.stderr.flush()
                    return m.group(1).upper()
            if verbose and reply:
                sys.stderr.write(reply.decode(errors="replace"))
                sys.stderr.flush()
            time.sleep(_ATTEMPT_GAP_S)
    finally:
        ser.close()
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Read an ambyte board's Wi-Fi STA MAC over the USB-C console.")
    parser.add_argument("--port", help="Serial port (e.g. COM7 or /dev/ttyACM0). "
                                       "Auto-detected if omitted.")
    parser.add_argument("--baud", type=int, default=_DEFAULT_BAUD,
                        help=f"Baud rate (default {_DEFAULT_BAUD}).")
    parser.add_argument("--verbose", action="store_true",
                        help="Echo the raw console output to stderr.")
    args = parser.parse_args(argv)

    try:
        import serial  # noqa: F401 — probe the dependency early for a clean error
    except ImportError:
        raise SystemExit("get_mac: pyserial not installed — `pip install pyserial`.")

    port = args.port or _autodetect_port()
    if args.verbose:
        sys.stderr.write(f"get_mac: using {port} @ {args.baud}\n")

    mac = read_mac(port, args.baud, verbose=args.verbose)
    if not mac:
        raise SystemExit(
            f"get_mac: no MAC seen on {port} within {_TOTAL_WINDOW_S:.0f}s "
            "(is the firmware running and the console idle?).")

    print(mac)
    return 0


if __name__ == "__main__":
    sys.exit(main())

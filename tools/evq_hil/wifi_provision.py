"""Credential-safe `wifi_join` for the Sprint 2 bench (replacement amendment A7).

    python wifi_provision.py --creds ~/.local/share/ambyte-write-integrity/secrets/wifi-bench.env \
        --log $PRIV/pre7-wifi.log [--wait 90]
    python wifi_provision.py --prompt --log ...          # local no-echo entry (getpass)

Why a dedicated tool: `wifi_join <ssid> <pass>` never logs the passphrase in the
firmware (v1.11.0 and C2 both), but the console ECHOES the typed line back over
serial, and the phase serial daemon writes every received byte to disk. So:
  * credentials come only from a 0600 file owned by this user (`SSID=` / `PASS=`
    lines) or a local getpass prompt - never argv, environment, chat or artifacts;
  * the port is opened exclusively here, with no daemon running (fuser empty);
  * every occurrence of the passphrase is redacted from the in-memory capture
    BEFORE anything is written, and the written log is re-checked for it;
  * only the result, AP association, IP and MQTT-connected lines are printed.
The bench identity comes from bench.py (AMBYTE_BENCH_MAC); there is no default board.
"""
from __future__ import annotations

import argparse
import getpass
import os
import re
import stat
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

REDACTED = b"<redacted-passphrase>"
SHOW = re.compile(rb"wifi_join \"|Associated with AP|got ip|IP_EVENT|MQTT connected|Wi-Fi disconnected|wifi_creds")
BAD_CHARS = set('"\\\n\r\t') | {"'"}


def load_creds(path: Path) -> tuple[str, str]:
    st = path.stat()
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or (st.st_mode & 0o077):
        raise SystemExit(f"{path}: must be a regular file owned by you with mode 0600")
    kv = {}
    for ln in path.read_text().splitlines():
        if "=" in ln and not ln.lstrip().startswith("#"):
            k, v = ln.split("=", 1)
            kv[k.strip()] = v.rstrip("\n")
    if not kv.get("SSID") or "PASS" not in kv:
        raise SystemExit(f"{path}: needs SSID= and PASS= lines")
    return kv["SSID"], kv["PASS"]


def check(ssid: str, pw: str) -> None:
    for name, v in (("SSID", ssid), ("PASS", pw)):
        if set(v) & BAD_CHARS:
            raise SystemExit(f"{name} contains a quote, backslash or control character; refusing (console quoting)")
    if not 1 <= len(ssid) <= 32 or not (8 <= len(pw) <= 63 or pw == ""):
        raise SystemExit("SSID must be 1..32 chars and PASS 8..63 chars (or empty for an open network)")


ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-_]")


def normalize(buf: bytes) -> bytes:
    """Console bytes as text lines: linenoise may redraw the echoed command with
    ANSI escapes BETWEEN characters, which would defeat a plain byte match, so
    escapes and carriage returns are stripped before redacting and writing."""
    return ANSI.sub(b"", buf).replace(b"\r", b"")


def redact(buf: bytes, pw: str, ssid: str = "") -> tuple[bytes, int]:
    """Passphrase AND SSID are removed (the credential file is "read privately,
    do not print"; the firmware echoes the SSID in `wifi_join "<ssid>": ...`)."""
    text = normalize(buf)
    if ssid:
        text = text.replace(ssid.encode(), b"<redacted-ssid>")
    if not pw:
        return text, 0
    n = text.count(pw.encode())
    return text.replace(pw.encode(), REDACTED), n


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--creds", type=Path)
    g.add_argument("--prompt", action="store_true")
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--wait", type=float, default=90.0)
    ap.add_argument("--settle", type=float, default=20.0)
    a = ap.parse_args()
    os.umask(0o077)
    if a.creds:
        ssid, pw = load_creds(a.creds.expanduser())
    else:
        ssid = input("SSID: ").strip()
        pw = getpass.getpass("Passphrase (not echoed): ")
    check(ssid, pw)
    port = bench.port()
    real = os.path.realpath(port)
    if subprocess.run(["fuser", real], capture_output=True).returncode == 0:
        raise SystemExit(f"{real} is in use: stop the serial daemon / esptool first")
    import serial

    out = bytearray()
    with serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True) as c:
        # dtr=1/rts=1 before open: the state serial_daemon.py uses, which neither
        # resets the ESP32-S3 USB-Serial-JTAG nor straps boot (r3.3 step 1 is
        # "on the running 1.11.0, with no reset"; console.py's dtr/rts-low open
        # DOES reset). The log is checked for a boot banner below.
        c.dtr = True
        c.rts = True
        c.port = port
        c.open()
        end = time.monotonic() + a.settle
        while time.monotonic() < end:
            out.extend(c.read(65536))
        c.write(f'wifi_join "{ssid}" "{pw}"\r\n'.encode())
        end = time.monotonic() + a.wait
        while time.monotonic() < end:
            out.extend(c.read(65536))
    safe, n_red = redact(bytes(out), pw, ssid)
    if (pw and (pw.encode() in safe or pw.encode() in normalize(safe))) or ssid.encode() in safe:
        raise SystemExit("redaction failed; nothing written")
    a.log.write_bytes(safe)
    os.chmod(a.log, 0o600)
    if (pw and pw.encode() in a.log.read_bytes()) or ssid.encode() in a.log.read_bytes():
        a.log.unlink()
        raise SystemExit("a credential was found in the written log; log removed")
    pw = ssid = ""                                # drop the references as early as possible
    booted = b"rst:0x" in safe or b"ESP-ROM:" in safe
    if booted:
        print("#WIFI WARNING: a boot banner is in the capture - the device reset during provisioning")
    for ln in safe.splitlines():
        if SHOW.search(ln):
            print(ln.decode(errors="replace")[:160])
    print(f"#WIFI log={a.log} passphrase_occurrences_redacted={n_red} identity={bench.mac()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compile-free flasher for the ambyte flash bundle, with a MAC allow-list gate.

Reads the connected board's base MAC (esptool read_mac), refuses to flash unless
that MAC is allow-listed in flash/allowed_macs.txt, then writes the prebuilt
images in bin/ with esptool. No PlatformIO project, ESP-IDF, or compiler needed.

This file is self-contained: it imports only the standard library and shells out
to esptool, so the whole flash/ folder can be handed over and run on its own.

Usage
-----
    python flash.py                 # auto-detect port, gate, confirm, flash
    python flash.py --port COM7     # explicit serial port
    python flash.py --yes           # skip the confirmation prompt
    python flash.py --name Roof-3   # set the payload device_name (skip the prompt)
    python flash.py --any           # bypass the allow-list (flash any board)
    python flash.py --list          # print the allow-list and exit
    python flash.py --dry-run       # read MAC + gate + preview, do not write

The thin launchers flash.cmd (Windows) and flash.sh (macOS/Linux) locate a
Python interpreter and forward their arguments here.

Device name
-----------
The flasher asks for a device name (the MQTT payload "device_name" field).
Leave it blank to keep the built-in default AMBYTE_<MAC> (baked into nvs.bin;
the firmware expands {MAC} on boot). Enter a name (e.g. "Roof-3") and, after
flashing, the script sets it on the booted board over the USB-Serial-JTAG
console (`cfg set device_name <name>` + reboot) — no NVS rebuild needed, so the
bundle stays compile-free. The MQTT client-id and topic-root are NOT affected;
they always stay AMBYTE_<MAC>.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = HERE / "bin"
ALLOWLIST = HERE / "allowed_macs.txt"

CHIP = "esp32s3"
BAUD = "460800"
# Espressif USB-Serial-JTAG (the native USB-C console/flash port). Used to
# auto-detect the port for the post-flash "set device_name" console step.
USB_JTAG_VID = 0x303A
# Firmware device_name buffer is char[64] -> 63 usable bytes.
MAX_NAME_LEN = 63
WRITE_FLASH_ARGS = ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"]
# (offset, filename in bin/) — mirrors the build's flasher_args.json + nvs@0x9000.
FLASH_FILES = [
    ("0x0",     "bootloader.bin"),
    ("0x8000",  "partitions.bin"),
    ("0x9000",  "nvs.bin"),
    ("0xf000",  "ota_data_initial.bin"),
    ("0x20000", "firmware.bin"),
]

_MAC_RE = re.compile(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def info(m: str) -> None:
    print(m, flush=True)


def warn(m: str) -> None:
    print(f"[warn] {m}", file=sys.stderr, flush=True)


def die(m: str, code: int = 1) -> "None":
    print(f"[error] {m}", file=sys.stderr, flush=True)
    sys.exit(code)


# ── esptool resolution ─────────────────────────────────────────────────────
def _pio_core() -> Path:
    return Path(os.environ.get("PLATFORMIO_CORE_DIR") or (Path.home() / ".platformio"))


def _module_ok(python: str, mod: str) -> bool:
    try:
        return subprocess.run([python, "-c", f"import {mod}"],
                              capture_output=True).returncode == 0
    except OSError:
        return False


def _pip_install(pkg: str) -> bool:
    attempts = [[sys.executable, "-m", "pip", "install", pkg]]
    uv = shutil.which("uv")
    if uv:
        attempts.append([uv, "pip", "install", "--python", sys.executable, pkg])
    for cmd in attempts:
        try:
            if subprocess.run(cmd).returncode == 0:
                return True
        except OSError:
            continue
    return False


def resolve_esptool() -> list[str]:
    """argv prefix that runs esptool.

    Prefer PlatformIO's penv python + its bundled esptool.py (zero installs; the
    penv python already carries pyserial). Otherwise fall back to the esptool
    module in the running interpreter, installing it on first use.
    """
    core = _pio_core()
    penv_py = next((p for p in (
        core / "penv" / "Scripts" / "python.exe",
        core / "penv" / "bin" / "python",
        core / "penv" / "bin" / "python3",
    ) if p.is_file()), None)

    esptool_py = None
    pkgs = core / "packages"
    if pkgs.is_dir():
        for d in sorted(pkgs.glob("tool-esptoolpy*"), reverse=True):
            cand = d / "esptool.py"
            if cand.is_file():
                esptool_py = cand
                break

    if penv_py and esptool_py:
        return [str(penv_py), str(esptool_py)]

    if _module_ok(sys.executable, "esptool"):
        return [sys.executable, "-m", "esptool"]
    info("esptool not found - installing it...")
    if _pip_install("esptool") and _module_ok(sys.executable, "esptool"):
        return [sys.executable, "-m", "esptool"]
    die("could not find or install esptool. Install PlatformIO, or run "
        f"`{sys.executable} -m pip install esptool`, then retry.")
    return []  # unreachable


# ── allow-list ─────────────────────────────────────────────────────────────
def load_allowlist() -> set[str]:
    """Bare uppercase MACs from allowed_macs.txt (AMBYTE_ prefix + # comments OK)."""
    if not ALLOWLIST.is_file():
        return set()
    macs: set[str] = set()
    for raw in ALLOWLIST.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = _MAC_RE.search(line)
        if m:
            macs.add(m.group(1).upper())
    return macs


# ── board MAC + flash ──────────────────────────────────────────────────────
def read_mac(esptool: list[str], port: str | None) -> str:
    cmd = list(esptool) + ["--chip", CHIP]
    if port:
        cmd += ["--port", port]
    cmd += ["read_mac"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=90,
                              env={**os.environ, "NO_COLOR": "1"})
    except subprocess.TimeoutExpired:
        die("esptool timed out reading the MAC. Close any serial monitor and retry.")
    except FileNotFoundError as exc:
        die(f"cannot launch esptool: {exc}")

    out = _ANSI_RE.sub("", (proc.stdout or "") + "\n" + (proc.stderr or ""))
    mac = None
    for line in out.splitlines():                 # prefer a MAC on a "MAC" line
        if "MAC" in line.upper():
            m = _MAC_RE.search(line)
            if m:
                mac = m.group(1).upper()
                break
    if not mac:
        m = _MAC_RE.search(out)
        mac = m.group(1).upper() if m else None
    if not mac:
        sys.stderr.write(out)
        die("esptool did not report a MAC. Is the board on USB-C and the port free "
            "(no serial monitor open)?")
    return mac  # type: ignore[return-value]


def do_flash(esptool: list[str], port: str | None) -> int:
    missing = [f for _, f in FLASH_FILES if not (BIN / f).is_file()]
    if missing:
        die(f"missing image(s) in {BIN}: {', '.join(missing)} — incomplete bundle.")
    cmd = list(esptool) + ["--chip", CHIP]
    if port:
        cmd += ["--port", port]
    cmd += ["--baud", BAUD, "--before", "default_reset", "--after", "hard_reset",
            "write_flash", *WRITE_FLASH_ARGS]
    for off, fn in FLASH_FILES:
        cmd += [off, fn]
    info("  Running: " + " ".join(cmd))
    return subprocess.run(cmd, cwd=str(BIN)).returncode


# ── device_name prompt + on-device set over the console ─────────────────────
def clean_device_name(name: str) -> str | None:
    """Return a validated device name, or None if empty/invalid.

    Accepts printable ASCII (space allowed) up to MAX_NAME_LEN bytes, minus
    quote/backslash (they complicate the console command line). None means
    "keep the default" (empty) or "rejected" (bad chars) — caller decides which.
    """
    n = name.strip()
    if not n:
        return None
    if len(n.encode("utf-8")) > MAX_NAME_LEN:
        return None
    for ch in n:
        if ch < " " or ord(ch) > 0x7E or ch in '"\\':
            return None
    return n


def prompt_device_name(default_note: str) -> str | None:
    """Ask for a device name. Returns a custom name, or None to keep the default."""
    try:
        raw = input(f"  Device name (blank = default {default_note}): ")
    except EOFError:
        return None
    if not raw.strip():
        return None
    cleaned = clean_device_name(raw)
    if cleaned is None:
        warn(f"invalid name (max {MAX_NAME_LEN} chars, printable ASCII, no quotes "
             "or backslashes) — keeping the default.")
        return None
    return cleaned


def _ensure_pyserial() -> None:
    """Make `import serial` work in this interpreter (install pyserial if needed)."""
    try:
        import serial  # noqa: F401
        return
    except ImportError:
        pass
    info("  pyserial not found — installing it (needed to set a custom name)...")
    if not _pip_install("pyserial"):
        die("could not install pyserial. Install it and retry, or set the name "
            "manually over the console:  cfg set device_name <name>")
    try:
        import serial  # noqa: F401
    except ImportError:
        die("pyserial still not importable after install.")


def _esp_jtag_ports() -> list[str]:
    """All Espressif USB-Serial-JTAG ports currently enumerated (VID 0x303A)."""
    from serial.tools import list_ports
    return sorted(p.device for p in list_ports.comports() if p.vid == USB_JTAG_VID)


def _open_console(port: str):
    """Open a USB-Serial-JTAG console port WITHOUT asserting DTR/RTS.

    esptool drives DTR/RTS to reset/bootloader-trip the chip; leaving them
    deasserted means merely opening the port never disturbs the running app.
    Baud is irrelevant on a USB CDC device but pyserial requires a value.
    """
    import serial
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def _try_set_name(ser, name: str) -> bool:
    """On an already-open console: wait for the `ambyte> ` prompt, send
    `cfg set device_name <name>`, confirm, then `reboot`. Returns True only on a
    confirmed set; False if this port isn't the live console (no prompt) or the
    command wasn't accepted. May raise serial errors (caller rescans)."""
    arg = f'"{name}"' if " " in name else name   # quote for esp_console arg split

    # An empty line makes linenoise re-print the prompt (we may have connected
    # after it was first shown). If we never see it, this isn't the live console.
    buf = ""
    end = time.time() + 6.0
    ser.write(b"\r\n")
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk.decode("utf-8", "replace")
            if "ambyte>" in buf:
                break
        else:
            ser.write(b"\r\n")   # nudge on silence -> linenoise reprints the prompt
    else:
        return False

    ser.reset_input_buffer()
    ser.write(f"cfg set device_name {arg}\r\n".encode("utf-8"))
    resp = ""
    ok = False
    end = time.time() + 5.0
    while time.time() < end:
        chunk = ser.read(256)
        if not chunk:
            continue
        resp += chunk.decode("utf-8", "replace")
        if "device_name" in resp and ("reboot to apply" in resp or "=" in resp):
            ok = True
            break
        if "not found" in resp.lower() or "unknown key" in resp.lower():
            break   # old firmware without `cfg`, or bad key — stop, don't hang
    for line in resp.splitlines():
        s = line.strip()
        if "device_name" in s and s:
            info("    " + s)
    if ok:
        ser.write(b"reboot\r\n")   # config is read at boot -> apply the new name
        time.sleep(0.5)
    return ok


def set_name_over_cli(port: str | None, name: str) -> bool:
    """Set device_name on the just-flashed board over its USB console.

    The ESP32-S3 native USB-Serial-JTAG re-enumerates to a NEW COM number on
    every reset (and leaves ghost ports behind), so we never trust the pre-flash
    port: we rescan for whichever Espressif JTAG port is live, opening each and
    keeping the one that answers with the prompt. We keep retrying across
    re-enumerations until a deadline. Best-effort — the flash already succeeded,
    so on failure we just print the one command to run by hand.
    """
    _ensure_pyserial()
    import serial

    arg = f'"{name}"' if " " in name else name
    manual = f"cfg set device_name {arg}"

    info("  Connecting to the console (USB-Serial-JTAG re-enumerates on reset)...")
    time.sleep(3.0)                       # let the reset settle + port re-appear
    deadline = time.time() + 45.0
    announced_wait = False
    while time.time() < deadline:
        # Try an explicit --port first (if given), then every live JTAG port —
        # the post-reset one has a different number than the flash port.
        cands = ([port] if port else []) + \
                [p for p in _esp_jtag_ports() if p != port]
        for cand in cands:
            try:
                ser = _open_console(cand)
            except (OSError, serial.SerialException):
                continue                  # ghost / not ready / busy
            try:
                if _try_set_name(ser, name):
                    info(f"  OK: device_name set to '{name}' and board rebooted.")
                    return True
            except (OSError, serial.SerialException):
                pass                      # port went stale mid-talk -> rescan
            finally:
                try:
                    ser.close()
                except Exception:
                    pass
        if not announced_wait:
            info("  ...waiting for the board's console to come up...")
            announced_wait = True
        time.sleep(1.0)

    warn("could not reach the console to set the name (the board's USB port keeps "
         "re-enumerating). The flash SUCCEEDED; once the board is up, set it by "
         f"hand over the console:  {manual}")
    return False


# ── main ───────────────────────────────────────────────────────────────────
def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Compile-free ambyte flasher with a MAC allow-list gate.")
    p.add_argument("--port", help="Serial port (e.g. COM7 or /dev/ttyACM0). "
                                   "Auto-detected if omitted.")
    p.add_argument("--yes", "-y", action="store_true",
                   help="Skip the confirmation prompt.")
    p.add_argument("--any", dest="any_board", action="store_true",
                   help="Bypass the allow-list and flash whatever board is connected.")
    p.add_argument("--name", default=None,
                   help="Payload device_name to set on the board after flashing "
                        "(skips the prompt). Omit for the default AMBYTE_<MAC>. "
                        "Does NOT affect the MQTT client-id / topic-root.")
    p.add_argument("--list", action="store_true",
                   help="Print the allow-list and exit.")
    p.add_argument("--dry-run", action="store_true",
                   help="Read MAC + gate + preview, but do not write.")
    args = p.parse_args(argv)

    # Validate --name up front (fails fast without a board attached).
    if args.name is not None and clean_device_name(args.name) is None:
        die(f"--name '{args.name}' is invalid (max {MAX_NAME_LEN} chars, "
            "printable ASCII, no quotes or backslashes).")

    allow = load_allowlist()

    if args.list:
        if not allow:
            info(f"Allow-list is empty or missing ({ALLOWLIST.name}).")
        else:
            info(f"Allow-list ({len(allow)} boards) from {ALLOWLIST.name}:")
            for m in sorted(allow):
                info(f"  AMBYTE_{m}")
        return 0

    esptool = resolve_esptool()

    info("Reading board MAC (esptool)...")
    mac = read_mac(esptool, args.port)
    name = f"AMBYTE_{mac}"

    if args.any_board:
        warn(f"--any: skipping the allow-list check for {name}.")
    elif not allow:
        die(f"allow-list {ALLOWLIST.name} is empty or missing — refusing to flash. "
            f"Add {name} to it, or pass --any to override.")
    elif mac not in allow:
        die(f"{name} is NOT in the allow-list ({ALLOWLIST.name}) — refusing to flash. "
            f"Add it there, or pass --any to override.")
    else:
        info(f"  {name} is allow-listed.")

    # Payload device_name: --name wins; else prompt when interactive; else keep
    # the default AMBYTE_<MAC> baked into nvs.bin. A custom name is applied over
    # the console after flashing (the MQTT client-id/topic-root stay AMBYTE_<MAC>).
    custom_name = None
    if args.name is not None:
        custom_name = clean_device_name(args.name)   # validated above
    elif not args.yes and not args.dry_run:
        custom_name = prompt_device_name(name)

    info("")
    info(f"  Will flash prebuilt firmware + provisioning to {name}:")
    for off, fn in FLASH_FILES:
        info(f"    {off:>8}  {fn}")
    if custom_name:
        info(f"    device_name -> {custom_name}  (set over console after flash)")
    else:
        info(f"    device_name -> {name}  (default)")
    info("")

    if args.dry_run:
        info("  --dry-run: not writing.")
        return 0
    if not args.yes:
        try:
            reply = input(f"  Flash {name}? [y/N] ").strip().lower()
        except EOFError:
            reply = ""
        if reply not in ("y", "yes"):
            info("  Declined.")
            return 0

    rc = do_flash(esptool, args.port)
    if rc != 0:
        warn(f"esptool write_flash failed (exit {rc}).")
        return rc
    info(f"  OK: flashed {name}.")

    if custom_name:
        info("")
        info(f"  Setting device_name '{custom_name}' over the console...")
        set_name_over_cli(args.port, custom_name)  # best-effort; hints on failure
    return 0


if __name__ == "__main__":
    sys.exit(main())

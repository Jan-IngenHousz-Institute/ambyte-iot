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
    python flash.py --any           # bypass the allow-list (flash any board)
    python flash.py --list          # print the allow-list and exit
    python flash.py --dry-run       # read MAC + gate + preview, do not write

The thin launchers flash.cmd (Windows) and flash.sh (macOS/Linux) locate a
Python interpreter and forward their arguments here.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = HERE / "bin"
ALLOWLIST = HERE / "allowed_macs.txt"

CHIP = "esp32s3"
BAUD = "460800"
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
    p.add_argument("--list", action="store_true",
                   help="Print the allow-list and exit.")
    p.add_argument("--dry-run", action="store_true",
                   help="Read MAC + gate + preview, but do not write.")
    args = p.parse_args(argv)

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

    info("")
    info(f"  Will flash prebuilt firmware + provisioning to {name}:")
    for off, fn in FLASH_FILES:
        info(f"    {off:>8}  {fn}")
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
    return 0


if __name__ == "__main__":
    sys.exit(main())

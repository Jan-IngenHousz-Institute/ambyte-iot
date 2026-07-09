#!/usr/bin/env python3
"""Fleet re-flash: full firmware + NVS, gated by a MAC allow-list.

For each connected ambyte board this script:
  1. reads the board's base MAC over the USB-C flash channel (esptool read_mac),
  2. refuses to touch it unless AMBYTE_<MAC> is in the allow-list below,
  3. provisions the *399b51c5…* device certificate bundle and sets the board's
     MQTT identity (mqtt_client_id AND device_name) to AMBYTE_<MAC>, then
  4. runs `pio run -t upload` — a full firmware + NVS re-flash.

It works on Windows and macOS, and installs its own Python requirements
(esptool, pyserial, and — if PlatformIO is missing — platformio) on first run.

Usage
-----
    # Flash the single connected board (auto-detect the port), asking to confirm:
    python tools/flash_certs.py

    # Fleet mode: flash board after board, confirming each; unplug when done,
    # plug the next in. Ctrl+C to stop.
    python tools/flash_certs.py --watch

    # Non-interactive (skip the per-board confirmation prompt):
    python tools/flash_certs.py --yes

    # Explicit port / dry run (do everything except the actual upload):
    python tools/flash_certs.py --port COM34
    python tools/flash_certs.py --dry-run

There are thin launchers alongside this file — flash_certs.cmd (Windows) and
flash_certs.sh (macOS/Linux) — that locate a Python interpreter and run this.

What gets flashed
-----------------
The full re-flash reuses the repo's normal provisioning path: `pio run -t upload`
builds the firmware and tools/extra_script.py generates the NVS image (offset
0x9000) from environment variables + .env. This script sets these variables so
the NVS carries the new identity/certs, and leaves everything else (MQTT
endpoint, topic root, Wi-Fi creds, device metadata) to your .env:

    AMBYTE_CERT_BUNDLE = 399b51c5…            (the new bundle folder)
    AMBYTE_DEV_CERT    = <bundle>/…cert.pem   (device certificate)
    AMBYTE_DEV_KEY     = <bundle>/…private.key(device private key)
    AMBYTE_CA_CERT     = AmazonRootCA1.pem    (the bundle ships no CA; sourced
                                               from the repo, else embedded)
    AMBYTE_CLIENT_ID   = AMBYTE_<MAC>         (baked literal — underscore)
    AMBYTE_DEVICE_NAME = AMBYTE_<MAC>         (baked literal — underscore)

The MAC is baked in literally (not left as a {MAC} token) so the identity is
exactly the allow-list string regardless of what firmware token-expansion the
board had before. The preview shown before each flash lists the endpoint/topic
that will be provisioned FROM YOUR .env — check them: a new certificate that
belongs to a different AWS IoT endpoint/experiment needs those .env values
updated before flashing, or the board will connect but fail to publish.
"""

from __future__ import annotations

import argparse
import csv
import importlib
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# ── Paths ────────────────────────────────────────────────────────────────────
TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
DOTENV    = REPO_ROOT / ".env"
FLASH_LOG = REPO_ROOT / "flashed_macs.csv"
CSV_HEADER = ["timestamp_utc", "mac", "port", "git_commit", "note"]

# ── Configuration ────────────────────────────────────────────────────────────
PIO_ENV     = "esp32-s3-devkitm-1"        # matches platformio.ini
CERT_BUNDLE = "399b51c5b32e82f5ff06f6d0a868b04ec5d31650c88fde6b38c7b7f610edf72b"
BUNDLE_DIR  = REPO_ROOT / "device_certs" / CERT_BUNDLE
CLIENT_PREFIX = "AMBYTE_"                  # mqtt name = AMBYTE_<MAC>

# ESP32-S3 native USB-Serial-JTAG identifiers (the USB-C console/flash port).
_ESP_USB_VID = 0x303A
_ESP_USB_JTAG_PID = 0x1001

# Only these boards may be flashed. Values are the AMBYTE_<MAC> strings the user
# provided; the leading "AMBYTE_" is stripped to the bare MAC when matching.
ALLOWED_AMBYTE = [
    "AMBYTE_E8:F6:0A:B1:1D:D4", "AMBYTE_E8:F6:0A:AF:84:A4", "AMBYTE_28:37:2F:FF:E7:BC",
    "AMBYTE_E8:F6:0A:B1:1C:B0", "AMBYTE_10:00:3B:72:21:58", "AMBYTE_28:37:2F:FF:E7:50",
    "AMBYTE_28:37:2F:FF:E7:18", "AMBYTE_28:37:2F:FF:E6:E4", "AMBYTE_E8:F6:0A:B1:78:04",
    "AMBYTE_28:37:2F:FF:E6:A4", "AMBYTE_E8:F6:0A:AF:A2:B8", "AMBYTE_E8:F6:0A:B1:EB:98",
    "AMBYTE_28:37:2F:FF:E6:A8", "AMBYTE_28:37:2F:FF:FD:44", "AMBYTE_E8:F6:0A:B1:1E:FC",
    "AMBYTE_E8:F6:0A:B1:1C:98", "AMBYTE_E8:F6:0A:B1:1D:18", "AMBYTE_E8:F6:0A:AE:FD:08",
    "AMBYTE_E8:F6:0A:B1:1D:0C", "AMBYTE_E8:F6:0A:AF:A2:A4", "AMBYTE_E8:F6:0A:B1:EC:44",
    "AMBYTE_E8:F6:0A:AE:BC:54", "AMBYTE_E8:F6:0A:B1:EB:BC", "AMBYTE_E8:F6:0A:AE:BB:E8",
    "AMBYTE_28:37:2F:FF:FD:0C", "AMBYTE_10:00:3B:72:21:7C", "AMBYTE_28:37:2F:FF:FC:34",
    "AMBYTE_10:00:3B:72:21:80", "AMBYTE_B4:3A:45:03:29:B0", "AMBYTE_B4:3A:45:03:29:5C",
    "AMBYTE_28:37:2F:FF:E7:34", "AMBYTE_28:37:2F:FF:FC:3C", "AMBYTE_28:37:2F:FF:E7:14",
    "AMBYTE_28:37:2F:FF:FC:38", "AMBYTE_E8:F6:0A:B1:1C:9C", "AMBYTE_D8:85:AC:9C:FB:98",
    "AMBYTE_E8:F6:0A:B1:EC:4C", "AMBYTE_E8:F6:0A:B1:1F:40", "AMBYTE_E8:F6:0A:B1:ED:AC",
    "AMBYTE_28:37:2F:FF:FC:04", "AMBYTE_E8:F6:0A:B1:1F:34",
]

# Amazon Root CA 1 — the new bundle ships no CA. Prefer a copy already in the
# repo; fall back to this embedded public root so the script is self-contained.
_AMAZON_ROOT_CA1 = """-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
"""

_MAC_RE = re.compile(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


# ── small utilities ──────────────────────────────────────────────────────────
def info(msg: str) -> None:
    print(msg, flush=True)


def warn(msg: str) -> None:
    print(f"[warn] {msg}", file=sys.stderr, flush=True)


def die(msg: str, code: int = 1) -> "NoReturn":  # type: ignore[name-defined]
    print(f"[error] {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def allowed_bare_macs() -> set[str]:
    """Return the allow-list as a set of bare, uppercase MACs (no AMBYTE_)."""
    out = set()
    for entry in ALLOWED_AMBYTE:
        mac = entry[len(CLIENT_PREFIX):] if entry.startswith(CLIENT_PREFIX) else entry
        out.add(mac.strip().upper())
    return out


def normalize_mac(raw: str) -> str | None:
    m = _MAC_RE.search(raw or "")
    return m.group(1).upper() if m else None


# ── dependency bootstrap ─────────────────────────────────────────────────────
def _pip_install(pip_name: str) -> bool:
    """Try, in order, every reasonable way to install `pip_name` for THIS
    interpreter. Returns True if at least one strategy exited 0. The repo's
    interpreter may be a uv-managed venv with no bundled pip, so uv and ensurepip
    are part of the ladder."""
    uv = shutil.which("uv")
    strategies: list[list[str]] = []
    strategies.append([sys.executable, "-m", "pip", "install", pip_name])
    if uv:
        # Install into the running interpreter's environment.
        strategies.append([uv, "pip", "install", "--python", sys.executable, pip_name])
    # Bootstrap pip, then retry pip.
    strategies.append([sys.executable, "-m", "ensurepip", "--upgrade"])

    for cmd in strategies:
        try:
            info(f"  installing: {' '.join(cmd)}")
            res = subprocess.run(cmd, cwd=str(REPO_ROOT))
        except Exception as exc:  # noqa: BLE001
            warn(f"install strategy failed to launch: {exc}")
            continue
        if res.returncode == 0 and cmd[-3:] == ["-m", "ensurepip", "--upgrade"]:
            # ensurepip succeeded — now actually install the package.
            try:
                res2 = subprocess.run(
                    [sys.executable, "-m", "pip", "install", pip_name],
                    cwd=str(REPO_ROOT))
                if res2.returncode == 0:
                    return True
            except Exception:  # noqa: BLE001
                pass
        elif res.returncode == 0:
            return True
    return False


def ensure_import(import_name: str, pip_name: str) -> None:
    """Ensure `import_name` is importable, installing `pip_name` if not."""
    try:
        importlib.import_module(import_name)
        return
    except ImportError:
        pass
    info(f"dependency '{pip_name}' missing - installing...")
    _pip_install(pip_name)
    importlib.invalidate_caches()
    try:
        importlib.import_module(import_name)
    except ImportError:
        die(f"could not install '{pip_name}'. Install it manually and re-run "
            f"(e.g. `uv pip install {pip_name}` or `pip install {pip_name}`).")


def ensure_deps() -> None:
    """Guarantee the pure-Python requirements are importable."""
    ensure_import("serial", "pyserial")
    ensure_import("esptool", "esptool")


def find_pio() -> list[str]:
    """Return an argv prefix that invokes PlatformIO, installing it if needed."""
    for exe in ("pio", "platformio"):
        found = shutil.which(exe)
        if found:
            return [found]

    # VS Code PlatformIO extension layout (~/.platformio/penv).
    penv = Path.home() / ".platformio" / "penv"
    for rel in ("Scripts/pio.exe", "Scripts/platformio.exe", "bin/pio", "bin/platformio"):
        cand = penv / rel
        if cand.is_file():
            return [str(cand)]

    # PlatformIO installed as a module in the current interpreter?
    try:
        importlib.import_module("platformio")
        return [sys.executable, "-m", "platformio"]
    except ImportError:
        pass

    # Last resort: install it for this interpreter.
    info("PlatformIO not found - installing (this pulls the toolchain on first build)...")
    _pip_install("platformio")
    importlib.invalidate_caches()
    try:
        importlib.import_module("platformio")
        return [sys.executable, "-m", "platformio"]
    except ImportError:
        die("PlatformIO is required for a full re-flash but could not be installed. "
            "Install it (https://platformio.org) and re-run.")
    return []  # unreachable


# ── serial port + MAC ────────────────────────────────────────────────────────
def list_esp_ports():
    """Return (jtag_ports, other_esp_ports) as lists of ListPortInfo."""
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    jtag = [p for p in ports if p.vid == _ESP_USB_VID and p.pid == _ESP_USB_JTAG_PID]
    other = [p for p in ports if p.vid == _ESP_USB_VID and p not in jtag]
    return jtag, other


def autodetect_port() -> str:
    jtag, other = list_esp_ports()
    if len(jtag) == 1:
        return jtag[0].device
    if len(jtag) > 1:
        listing = ", ".join(p.device for p in jtag)
        die(f"multiple boards found ({listing}). Pass --port to pick one, "
            "or use --watch to flash them one at a time.")
    if other:
        hint = ", ".join(f"{p.device} ({p.description})" for p in other)
        die("no ESP32-S3 USB-Serial-JTAG port found. Espressif devices seen on "
            f"other interfaces: {hint}. Connect the USB-C (native USB) port, or "
            "pass --port.")
    die("no board found. Is it connected by USB-C and powered?")
    return ""  # unreachable


def read_mac(port: str) -> str:
    """Read the base MAC via esptool. Returns an uppercase colon MAC or dies."""
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3",
           "--port", port, "read_mac"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=90,
                              env={**os.environ, "NO_COLOR": "1"})
    except subprocess.TimeoutExpired:
        die(f"esptool timed out reading the MAC on {port}.")
    except FileNotFoundError as exc:
        die(f"cannot launch esptool: {exc}")

    output = _ANSI_RE.sub("", (proc.stdout or "") + "\n" + (proc.stderr or ""))
    # esptool prints e.g. "MAC: e8:f6:0a:b1:1d:d4" (or "BASE MAC:"). Take the
    # first MAC-shaped token on a MAC line to avoid picking up USB ids.
    mac = None
    for line in output.splitlines():
        if "MAC" in line.upper():
            mac = normalize_mac(line)
            if mac:
                break
    if not mac:
        mac = normalize_mac(output)
    if not mac:
        sys.stderr.write(output)
        die(f"esptool did not report a MAC on {port} (exit {proc.returncode}). "
            "Is the port free (close any serial monitor) and the board in reach?")
    return mac


# ── cert + env plumbing ──────────────────────────────────────────────────────
def _rel(p: Path) -> str:
    """Repo-relative POSIX path when possible (build_nvs_image resolves those
    against the repo root); else absolute."""
    try:
        return p.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(p.resolve())


def resolve_cert_paths() -> tuple[str, str, str]:
    """Return (ca_cert, dev_cert, dev_key) paths for the bundle. Dies if the
    device cert/key are missing; the CA is sourced from the repo or an embedded
    fallback written next to the bundle."""
    if not BUNDLE_DIR.is_dir():
        die(f"cert bundle not found: {BUNDLE_DIR}")

    def pick(suffixes: tuple[str, ...]) -> Path | None:
        for f in sorted(BUNDLE_DIR.iterdir()):
            if f.name.startswith("."):
                continue
            low = f.name.lower()
            if any(low.endswith(s) for s in suffixes):
                return f
        return None

    dev_cert = pick((".cert.pem", "-certificate.pem.crt", ".crt"))
    dev_key  = pick((".private.key", "-private.pem.key"))
    if not dev_key:  # last resort: any *.key that isn't the public key
        for f in sorted(BUNDLE_DIR.iterdir()):
            if f.name.lower().endswith(".key") and "public" not in f.name.lower():
                dev_key = f
                break
    if not dev_cert:
        die(f"no device certificate (*.cert.pem / *.crt) in {BUNDLE_DIR}")
    if not dev_key:
        die(f"no device private key (*.private.key) in {BUNDLE_DIR}")

    # CA: the new bundle ships no CA. Resolution order, least-invasive first:
    #   1. explicit .env AMBYTE_CA_CERT override,
    #   2. a CA already sitting in the new bundle,
    #   3. an existing repo CA (used in place — no copy),
    #   4. last resort: write the embedded Amazon Root CA 1 into the bundle.
    ca_env = _dotenv_value("AMBYTE_CA_CERT")
    if ca_env:
        ca_path = Path(ca_env)
        if not ca_path.is_absolute():
            ca_path = REPO_ROOT / ca_path
    else:
        bundle_ca = BUNDLE_DIR / "AmazonRootCA1.pem"
        repo_ca = (REPO_ROOT / "device_certs" /
                   "dom_ludo_prototype_ambyte_thing_v2" / "AmazonRootCA1.pem")
        if bundle_ca.is_file():
            ca_path = bundle_ca
        elif repo_ca.is_file():
            ca_path = repo_ca
        else:
            bundle_ca.write_text(_AMAZON_ROOT_CA1, encoding="utf-8")
            info(f"  CA: wrote embedded Amazon Root CA 1 -> {_rel(bundle_ca)}")
            ca_path = bundle_ca
    if not ca_path.is_file():
        die(f"CA certificate not found: {ca_path}")

    return _rel(ca_path), _rel(dev_cert), _rel(dev_key)


def _dotenv_value(key: str) -> str | None:
    """Read a single KEY from .env (shell env wins). Minimal, for preview only."""
    if os.environ.get(key):
        return os.environ[key]
    if not DOTENV.exists():
        return None
    for raw in DOTENV.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        if k.strip() == key:
            return v.strip().strip('"').strip("'")
    return None


def build_pio_env(mac: str, ca: str, dev_cert: str, dev_key: str) -> dict[str, str]:
    name = f"{CLIENT_PREFIX}{mac}"          # AMBYTE_<MAC>, literal (underscore)
    env = dict(os.environ)
    env["AMBYTE_CERT_BUNDLE"] = CERT_BUNDLE
    env["AMBYTE_CA_CERT"]     = ca
    env["AMBYTE_DEV_CERT"]    = dev_cert
    env["AMBYTE_DEV_KEY"]     = dev_key
    env["AMBYTE_CLIENT_ID"]   = name
    env["AMBYTE_DEVICE_NAME"] = name
    return env


# ── logging ──────────────────────────────────────────────────────────────────
def git_commit() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             cwd=str(REPO_ROOT), capture_output=True, text=True,
                             timeout=5)
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:  # noqa: BLE001
        return ""


def append_log(mac: str, port: str, note: str) -> None:
    new_file = not FLASH_LOG.exists()
    row = {
        "timestamp_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "mac": mac, "port": port, "git_commit": git_commit(), "note": note,
    }
    with FLASH_LOG.open("a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=CSV_HEADER)
        if new_file:
            w.writeheader()
        w.writerow(row)


# ── flash one board ──────────────────────────────────────────────────────────
def flash_one(port: str, pio: list[str], args) -> tuple[str, str | None]:
    """Read MAC, gate on the allow-list, provision + full re-flash one board.
    Returns (status, mac) where status is 'flashed'|'skipped'|'declined'|'failed'
    (mac is None only if the MAC could not be read)."""
    info("")
    info(f"== Board on {port} ==")
    info("Reading MAC (esptool)...")
    mac = read_mac(port)
    name = f"{CLIENT_PREFIX}{mac}"

    if mac not in allowed_bare_macs():
        warn(f"{name} is NOT in the allow-list - refusing to flash. Skipping.")
        return "skipped", mac

    ca, dev_cert, dev_key = resolve_cert_paths()
    endpoint = _dotenv_value("AMBYTE_MQTT_URI") or "(from Kconfig default)"
    topic    = _dotenv_value("AMBYTE_TOPIC_ROOT") or "(from Kconfig default)"

    info("")
    info("  Will provision + FULL re-flash (firmware + NVS):")
    info(f"    MAC              : {mac}   (allow-listed)")
    info(f"    mqtt_client_id   : {name}")
    info(f"    device_name      : {name}")
    info(f"    cert bundle      : {CERT_BUNDLE[:12]}...")
    info(f"    device cert      : {dev_cert}")
    info(f"    device key       : {dev_key}")
    info(f"    CA cert          : {ca}")
    info(f"    MQTT endpoint    : {endpoint}       (from .env - verify!)")
    info(f"    topic root       : {topic}       (from .env - {{MAC}} expands on-device)")
    info("")

    if args.dry_run:
        info("  --dry-run: not uploading.")
        return "declined", mac

    if not args.yes:
        try:
            reply = input(f"  Flash {name} on {port}? [y/N] ").strip().lower()
        except EOFError:
            reply = ""
        if reply not in ("y", "yes"):
            info("  Declined.")
            return "declined", mac

    env = build_pio_env(mac, ca, dev_cert, dev_key)
    cmd = pio + ["run", "-e", PIO_ENV, "-t", "upload", "--upload-port", port]
    info(f"  Running: {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=str(REPO_ROOT), env=env)
    if proc.returncode != 0:
        warn(f"upload FAILED for {name} (pio exit {proc.returncode}).")
        return "failed", mac

    append_log(mac, port, f"flash_certs bundle={CERT_BUNDLE[:12]} mqtt={name}")
    info(f"  OK: flashed {name} and logged to {FLASH_LOG.name}")
    return "flashed", mac


# ── watch loop (fleet) ───────────────────────────────────────────────────────
# One board is handled at a time. Both reading the MAC and the pio upload reset
# the ESP32-S3, and because the console runs on the SoC's native USB-Serial-JTAG
# the port DISAPPEARS for ~1-2 s and re-enumerates on the same VID/PID. So
# "board removed" must be distinguished from that reset bounce by requiring the
# port to stay absent for a sustained window before we accept the next board;
# otherwise the just-flashed board would be re-detected and flashed again.
_REMOVAL_CONFIRM_S = 4.0    # continuous absence that counts as a real unplug
_POLL_S = 0.5
_SETTLE_S = 1.0             # after a board appears, let enumeration finish


def _jtag_present(port: str) -> bool:
    return any(p.device == port for p in list_esp_ports()[0])


def wait_for_board() -> str:
    """Block until exactly one JTAG board is present; return its port."""
    info("")
    info("Waiting for a board (plug one in via USB-C)... Ctrl+C to stop.")
    while True:
        jtag, _ = list_esp_ports()
        if len(jtag) == 1:
            return jtag[0].device
        if len(jtag) > 1:
            warn(f"multiple boards ({', '.join(p.device for p in jtag)}); "
                 "leave just one connected.")
        time.sleep(1.0)


def wait_for_removal(port: str) -> None:
    """Block until `port` has been absent continuously for _REMOVAL_CONFIRM_S,
    so a post-flash USB reset (brief absence) is not mistaken for an unplug."""
    info(f"  Unplug the board on {port} to continue (Ctrl+C to stop)...")
    absent_for = 0.0
    while True:
        if _jtag_present(port):
            absent_for = 0.0                   # present -> reset the absence timer
        else:
            absent_for += _POLL_S
            if absent_for >= _REMOVAL_CONFIRM_S:
                return
        time.sleep(_POLL_S)


def watch_loop(pio: list[str], args) -> int:
    tally = {"flashed": 0, "skipped": 0, "declined": 0, "failed": 0}
    total = len(allowed_bare_macs())
    try:
        while True:
            port = wait_for_board()
            time.sleep(_SETTLE_S)               # let enumeration finish
            # A per-board failure (die() -> SystemExit, or any exception) must
            # NOT kill the whole fleet run: tally it as failed and move on.
            try:
                status, _mac = flash_one(port, pio, args)
            except SystemExit as exc:
                warn(f"board on {port} errored ({exc.code}); skipping to next.")
                status = "failed"
            except Exception as exc:            # noqa: BLE001
                warn(f"unexpected error on {port}: {exc}; skipping to next.")
                status = "failed"
            tally[status] = tally.get(status, 0) + 1
            info(f"  [session tally] flashed={tally['flashed']} "
                 f"skipped={tally['skipped']} declined={tally['declined']} "
                 f"failed={tally['failed']}  (allow-list has {total})")
            wait_for_removal(port)
    except KeyboardInterrupt:
        info("")
        info(f"Stopped. flashed={tally['flashed']} skipped={tally['skipped']} "
             f"declined={tally['declined']} failed={tally['failed']}")
    return 0


# ── main ─────────────────────────────────────────────────────────────────────
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Full firmware+NVS re-flash of allow-listed ambyte boards "
                    "with the 399b51c5 cert bundle and mqtt name AMBYTE_<MAC>.")
    parser.add_argument("--port", help="Serial port (e.g. COM34 or /dev/cu.usbmodem…). "
                                       "Auto-detected if omitted.")
    parser.add_argument("--watch", action="store_true",
                        help="Fleet mode: flash boards one after another.")
    parser.add_argument("--yes", "-y", action="store_true",
                        help="Skip the per-board confirmation prompt.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Read MAC + check allow-list + preview, but do not upload.")
    parser.add_argument("--list", action="store_true",
                        help="Print the allow-list and exit.")
    args = parser.parse_args(argv)

    if args.list:
        macs = sorted(allowed_bare_macs())
        info(f"Allow-list ({len(macs)} boards):")
        for m in macs:
            info(f"  {CLIENT_PREFIX}{m}")
        return 0

    info("ambyte flash_certs - ensuring requirements...")
    ensure_deps()
    if args.dry_run:
        pio: list[str] = []          # no upload happens; don't install PlatformIO
    else:
        pio = find_pio()
        info(f"Using PlatformIO: {' '.join(pio)}")

    if args.watch:
        return watch_loop(pio, args)

    port = args.port or autodetect_port()
    status, _ = flash_one(port, pio, args)
    return 0 if status in ("flashed", "declined") else 1


if __name__ == "__main__":
    sys.exit(main())

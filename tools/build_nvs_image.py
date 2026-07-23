#!/usr/bin/env python3
"""
Build an NVS partition binary that pre-provisions a device.

Reads .env + a device_certs/<bundle>/ directory (via _prov_env) and produces
an ESP-IDF NVS image containing every value the firmware would otherwise
collect over BLE. Flashing this image to NVS (offset 0x9000) before first
boot lets the device come up fully provisioned with no BLE round-trip.

Wire-up:
  - tools/extra_script.py invokes this from PlatformIO before upload.
  - Manual run:  uv run python tools/build_nvs_image.py --out /tmp/nvs.bin

NVS layout (mirrors the firmware's nvs_open() namespaces):
  namespace    key               source
  ─────────────────────────────────────────────────────────────────
  device_cfg   mqtt_uri          AMBYTE_MQTT_URI
               mqtt_client_id    AMBYTE_CLIENT_ID
               mqtt_topic_root   AMBYTE_TOPIC_ROOT
               device_id         AMBYTE_DEVICE_ID
               protocol_id       AMBYTE_PROTOCOL_ID
               device_name       AMBYTE_DEVICE_NAME
               device_ver        AMBYTE_DEVICE_VERSION   (NVS key max 15 ch)
               device_firm       AMBYTE_DEVICE_FIRMWARE
               firmware_ver      AMBYTE_FIRMWARE_VERSION
               cmd_topic         AMBYTE_COMMAND_TOPIC    (optional; Stage-2 cmd in)
               status_topic      AMBYTE_STATUS_TOPIC     (optional; Stage-2 reply out)
               timezone          AMBYTE_TIMEZONE         (IANA name; defaults to
                                                          Europe/Amsterdam)
               heartbeat_s       AMBYTE_HEARTBEAT_S      (optional; u32 seconds)
               flash_time        <image build time>      (u32 epoch; RTC fallback)
  certs        ca_cert           file at AMBYTE_CA_CERT
               dev_cert          file at AMBYTE_DEV_CERT
               dev_key           file at AMBYTE_DEV_KEY
  wifi_prov    provisioned       1                       (u8, gates app_main)
  wifi_creds   ssid              AMBYTE_SSID             (consumed once at boot)
               pass              AMBYTE_PASSWORD

Exits non-zero on any missing required value so a misconfigured .env fails
loudly at build time instead of bricking the device.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _prov_env import load_dotenv, resolve_cert_bundle, REPO_ROOT  # noqa: E402

# NVS partition size — must match partitions.csv ("nvs, ..., 0x9000, 0x6000").
NVS_PARTITION_SIZE = 0x6000

# (env_var, namespace, nvs_key, kind)
# kind = "string" — plain UTF-8 string, value taken from env var
#      = "file"   — value is a file path; PEM contents will be inlined
#      = "u8"     — small integer literal
FIELDS = [
    ("AMBYTE_MQTT_URI",         "device_cfg", "mqtt_uri",        "string"),
    ("AMBYTE_CLIENT_ID",        "device_cfg", "mqtt_client_id",  "string"),
    ("AMBYTE_TOPIC_ROOT",       "device_cfg", "mqtt_topic_root", "string"),
    ("AMBYTE_DEVICE_ID",        "device_cfg", "device_id",       "string"),
    ("AMBYTE_PROTOCOL_ID",      "device_cfg", "protocol_id",     "string"),
    ("AMBYTE_DEVICE_NAME",      "device_cfg", "device_name",     "string"),
    ("AMBYTE_DEVICE_VERSION",   "device_cfg", "device_ver",      "string"),
    ("AMBYTE_DEVICE_FIRMWARE",  "device_cfg", "device_firm",     "string"),
    ("AMBYTE_FIRMWARE_VERSION", "device_cfg", "firmware_ver",    "string"),
    ("AMBYTE_CA_CERT",          "certs",      "ca_cert",         "file"),
    ("AMBYTE_DEV_CERT",         "certs",      "dev_cert",        "file"),
    ("AMBYTE_DEV_KEY",          "certs",      "dev_key",         "file"),
    ("AMBYTE_SSID",             "wifi_creds", "ssid",            "string"),
    ("AMBYTE_PASSWORD",         "wifi_creds", "pass",            "string"),
]

# Optional fields — included only when set in .env; absence is not an error.
# The inbound command topic (device subscribes) + reply topic (device publishes)
# for the Stage-2 command channel. Full authorized topics, deployment-specific.
OPTIONAL_FIELDS = [
    ("AMBYTE_COMMAND_TOPIC",    "device_cfg", "cmd_topic",       "string"),
    ("AMBYTE_STATUS_TOPIC",     "device_cfg", "status_topic",    "string"),
    # IANA timezone name. Drives DST-aware on-device scheduling (sched.lua
    # sun/day-night/clock jobs, via components/timezone) AND is echoed in the
    # MQTT envelope so the cloud derives local-time columns. Defaults to
    # DEFAULT_TIMEZONE below when unset.
    ("AMBYTE_TIMEZONE",         "device_cfg", "timezone",        "string"),
    # STATUS heartbeat period in seconds (firmware default 300; 0 disables).
    ("AMBYTE_HEARTBEAT_S",      "device_cfg", "heartbeat_s",     "u32"),
]

# Baked when AMBYTE_TIMEZONE is unset, so a fresh flash schedules in NL local
# time from first boot — even with flash.py --no-provision, or if the console
# provisioning step is skipped — instead of falling back to the firmware's fixed
# offset. Mirrors flash/flash.py's DEFAULT_TIMEZONE; override via AMBYTE_TIMEZONE.
DEFAULT_TIMEZONE = "Europe/Amsterdam"


def _find_idf() -> Path:
    idf = os.environ.get("IDF_PATH") or os.path.expanduser(
        "~/.platformio/packages/framework-espidf")
    p = Path(idf)
    if not p.is_dir():
        raise SystemExit(
            f"IDF_PATH not found at {p} — set IDF_PATH or install PlatformIO's "
            "espressif32 platform.")
    return p


# ---------------------------------------------------------------------------
# NVS generator Python resolution
#
# ESP-IDF 5.5+ ships nvs_partition_gen.py as a thin wrapper around the pip
# package below, which lives only in IDF's own venv. PlatformIO's outer penv —
# and a uv/pyenv interpreter that happens to be running the build — do not have
# it. We probe every known venv layout, pick the first interpreter that can
# *actually import* the package, and (unless opted out) install it if nothing
# has it yet.
# ---------------------------------------------------------------------------

_NVS_MODULE = "esp_idf_nvs_partition_gen"   # importable module name
_NVS_PYPI   = "esp-idf-nvs-partition-gen"   # PyPI project name


def _truthy(value: str | None) -> bool:
    return (value or "").strip().lower() in ("1", "true", "yes", "on")


def _has_nvs_module(python: str) -> bool:
    """True if `python` can import the NVS-generator package."""
    try:
        r = subprocess.run([python, "-c", f"import {_NVS_MODULE}"],
                           capture_output=True, text=True)
    except OSError:
        return False
    return r.returncode == 0


def _idf_python_candidates() -> list[Path]:
    """Ordered Python-interpreter paths that might host the NVS package.

    Covers the three ways ESP-IDF's Python env lands on a machine:
      1. IDF_PYTHON_ENV_PATH — set by IDF's export.{sh,ps1}; authoritative.
      2. PlatformIO layout    — <core>/penv/.espidf-X.Y.Z/ (newest first,
         honouring PLATFORMIO_CORE_DIR over the ~/.platformio default).
      3. Standalone IDF        — ~/.espressif/python_env/idfX.Y_pyZ.Z_env/.
    """
    candidates: list[Path] = []

    def add(base: Path) -> None:
        candidates.append(base / "Scripts" / "python.exe")   # Windows
        candidates.append(base / "bin" / "python")           # POSIX

    env_path = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_path:
        add(Path(env_path))

    pio_core = Path(os.environ.get("PLATFORMIO_CORE_DIR")
                    or os.path.expanduser("~/.platformio"))
    pio_penv = pio_core / "penv"
    if pio_penv.is_dir():
        # Newest IDF first — .espidf-5.5.0 beats a leftover .espidf-4.4.7.
        for child in sorted(pio_penv.glob(".espidf-*"), reverse=True):
            add(child)

    espressif = Path(os.path.expanduser("~/.espressif/python_env"))
    if espressif.is_dir():
        for child in sorted(espressif.glob("idf*_env"), reverse=True):
            add(child)

    return candidates


def _pip_install_nvs(python: str) -> bool:
    """Install the NVS package into `python`. Try its own pip, then uv."""
    attempts = [[python, "-m", "pip", "install",
                 "--disable-pip-version-check", _NVS_PYPI]]
    uv = shutil.which("uv")
    if uv:
        attempts.append([uv, "pip", "install", "--python", python, _NVS_PYPI])
    for cmd in attempts:
        try:
            r = subprocess.run(cmd, capture_output=True, text=True)
        except OSError:
            continue
        if r.returncode == 0:
            return True
        sys.stderr.write(r.stdout)
        sys.stderr.write(r.stderr)
    return False


def _missing_module_help(python: str) -> str:
    return (
        f"nvs_partition_gen.py needs the '{_NVS_MODULE}' package, which "
        f"{python} cannot import.\nInstall it and re-build:\n"
        f"  {python} -m pip install {_NVS_PYPI}\n"
        f"or, with uv:\n"
        f"  uv pip install --python {python} {_NVS_PYPI}")


def _find_idf_python() -> str:
    """Return a Python interpreter that can import the NVS-generator package.

    Probes every known IDF-venv layout and returns the first interpreter that
    *actually imports* the package — a file merely existing is not enough (an
    older .espidf-5.3.1 has nvs_partition_gen.py but not this package). If
    nothing has it, installs it into the best target (a real IDF venv when one
    exists, else the running interpreter) and returns that. Set
    AMBYTE_NVS_NO_AUTOINSTALL=1 to skip the install and fail with instructions.
    """
    existing: list[str] = []
    for candidate in _idf_python_candidates():
        if candidate.is_file():
            path = str(candidate)
            if _has_nvs_module(path):
                return path
            existing.append(path)

    # A user may have installed the package into the interpreter running the
    # build (e.g. `uv run pio run`); accept that before trying to install.
    if _has_nvs_module(sys.executable):
        return sys.executable

    # Nothing has it. Prefer a real IDF venv as the install home (persists per
    # IDF version, benefits every project); else the running interpreter.
    target = existing[0] if existing else sys.executable

    if _truthy(os.environ.get("AMBYTE_NVS_NO_AUTOINSTALL")):
        raise SystemExit(_missing_module_help(target))

    print(f"ambyte-nvs: '{_NVS_MODULE}' not found - installing {_NVS_PYPI} "
          f"into {target}", file=sys.stderr)
    if _pip_install_nvs(target) and _has_nvs_module(target):
        return target

    raise SystemExit(_missing_module_help(target))


def _read_pem(path_str: str) -> str:
    """Resolve a (possibly relative) path against the repo root and return its text."""
    p = Path(path_str)
    if not p.is_absolute():
        p = REPO_ROOT / p
    if not p.is_file():
        raise SystemExit(f"cert file not found: {p}")
    return p.read_text()


def _quote_csv(value: str) -> str:
    r"""CSV-quote a single value (RFC 4180): wrap in "..." and double internal quotes.
    Required because PEM contents contain newlines."""
    return '"' + value.replace('"', '""') + '"'


def _collect_values() -> dict[tuple[str, str], tuple[str, str]]:
    """Return {(namespace, key): (kind, value)} after resolving every field.
    Raises SystemExit on missing required values."""
    out: dict[tuple[str, str], tuple[str, str]] = {}
    missing: list[str] = []
    for env_var, ns, key, kind in FIELDS:
        raw = os.environ.get(env_var)
        if raw is None or raw == "":
            missing.append(env_var)
            continue
        if kind == "file":
            out[(ns, key)] = ("string", _read_pem(raw))
        else:
            out[(ns, key)] = (kind, raw)

    if missing:
        raise SystemExit(
            "missing required value(s):\n  - " + "\n  - ".join(missing)
            + "\nSet them in .env (or AMBYTE_NVS_SKIP=1 to flash stock firmware).")

    # Optional fields: include only when present, never error on absence.
    for env_var, ns, key, kind in OPTIONAL_FIELDS:
        raw = os.environ.get(env_var)
        if raw:
            out[(ns, key)] = (kind, raw)

    # Timezone default: bake Europe/Amsterdam when unset so on-device scheduling
    # is DST-correct from first boot. setdefault → an explicit AMBYTE_TIMEZONE
    # (added just above from .env/shell) always wins.
    out.setdefault(("device_cfg", "timezone"), ("string", DEFAULT_TIMEZONE))

    # provisioned=1 is constant; not driven by env.
    out[("wifi_prov", "provisioned")] = ("u8", "1")

    # Image build time (UTC epoch). The firmware sets the RTC from this at boot
    # when the RTC is invalid or behind it — a no-touch clock bootstrap accurate
    # to the build→boot delay (the image is regenerated on every build). A
    # correct RTC is never moved: it always reads ahead of this timestamp.
    out[("device_cfg", "flash_time")] = ("u32", str(int(time.time())))
    return out


def _write_csv(values: dict[tuple[str, str], tuple[str, str]], csv_path: Path) -> None:
    """Write the NVS-partition-generator CSV, with PEMs inlined.

    Groups keys by namespace so each namespace is declared exactly once — the NVS
    generator treats a repeated `namespace` line as a *separate* namespace, so
    re-declaring (e.g. appending optional device_cfg keys after certs/wifi_creds)
    would hide those keys from nvs_open(<name>)."""
    by_ns: dict[str, list[tuple[str, str, str]]] = {}
    ns_order: list[str] = []
    for (ns, key), (enc, value) in values.items():
        if ns not in by_ns:
            by_ns[ns] = []
            ns_order.append(ns)
        by_ns[ns].append((key, enc, value))

    lines: list[str] = ["key,type,encoding,value"]
    for ns in ns_order:
        lines.append(f"{ns},namespace,,")
        for key, enc, value in by_ns[ns]:
            if enc in ("u8", "u32"):
                lines.append(f"{key},data,{enc},{value}")
            else:
                lines.append(f"{key},data,string,{_quote_csv(value)}")
    csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _run_generator(idf_path: Path, csv_path: Path, out_path: Path) -> None:
    generator = (idf_path / "components" / "nvs_flash"
                 / "nvs_partition_generator" / "nvs_partition_gen.py")
    if not generator.is_file():
        raise SystemExit(f"nvs_partition_gen.py not found at {generator}")

    python = _find_idf_python()
    cmd = [
        python, str(generator), "generate",
        str(csv_path), str(out_path), hex(NVS_PARTITION_SIZE),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(
            f"nvs_partition_gen.py failed (exit {result.returncode}) using "
            f"{python}.")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True,
                   help="output NVS binary path (e.g. .pio/build/<env>/nvs.bin)")
    p.add_argument("--csv", type=Path, default=None,
                   help="write the intermediate CSV here (default: alongside --out)")
    p.add_argument("--quiet", action="store_true",
                   help="suppress success message; only print the output path")
    args = p.parse_args()

    load_dotenv()
    resolve_cert_bundle()

    values = _collect_values()

    out_path = args.out.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    csv_path = (args.csv or out_path.with_suffix(".csv")).resolve()

    _write_csv(values, csv_path)
    _run_generator(_find_idf(), csv_path, out_path)

    if not args.quiet:
        ns_keys: dict[str, list[str]] = {}
        for ns, key in values.keys():
            ns_keys.setdefault(ns, []).append(key)
        print(f"NVS image written: {out_path} ({NVS_PARTITION_SIZE} bytes)")
        for ns, keys in ns_keys.items():
            print(f"  [{ns}]  {', '.join(keys)}")
    print(out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())

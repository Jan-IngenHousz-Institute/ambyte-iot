#!/usr/bin/env python3
"""Fail closed on the published artifact; pre-signing checks are separate."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from flash_gui.firmware_metadata import ImageError, MAX_SIGNED_BYTES, inspect_image, key_id, padded_intermediate, NO_EFUSE, REQUIRED, check_config

BENCH_KEY = ROOT / "tools/keys/UNSAFE_BENCH_ONLY_firmware.pem"
RELEASE_PIN = ROOT / "tools/keys/release_firmware.keyid"
FIRST_SIGNED_RELEASE = ROOT / "tools/keys/first_signed_release.version"


def private_key(data: bytes):
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    try:
        return load_pem_private_key(data, password=None)
    except (ValueError, TypeError) as exc:
        raise ImageError("invalid signing key PEM") from exc


def bench_id() -> str:
    return key_id(private_key(BENCH_KEY.read_bytes()))


def load_pin(path: Path = RELEASE_PIN) -> str:
    try:
        pin = path.read_text().strip()
    except OSError as exc:
        raise ImageError("release key pin absent; Dominik must commit the reviewed key ID") from exc
    if not re.fullmatch(r"fw-rsa3072:[0-9a-f]{64}", pin) or pin.endswith("0" * 64):
        raise ImageError("release key pin unconfigured/invalid; commit the reviewed real key ID")
    if pin == bench_id():
        raise ImageError("public bench key must never become the release root")
    return pin


def load_boundary(path: Path = FIRST_SIGNED_RELEASE) -> str:
    try:
        version = path.read_text().strip()
    except OSError as exc:
        raise ImageError("first signed release boundary absent; configure it with the reviewed key ID") from exc
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version) or version == "0.0.0":
        raise ImageError("first signed release boundary UNCONFIGURED/invalid; configure it with the reviewed key ID")
    return version



def reject_private_material(data: bytes) -> None:
    # mbedTLS itself contains PEM boundary strings. A marker alone is not key
    # material; require the bounded base64 body and matching end marker.
    if re.search(rb"-----BEGIN (?:RSA )?PRIVATE KEY-----\s+[A-Za-z0-9+/=\r\n]{128,}\s+-----END (?:RSA )?PRIVATE KEY-----", data) or BENCH_KEY.read_bytes().strip() in data:
        raise ImageError("private key material leaked into an artifact")


def check_release(data: bytes, pin: str) -> None:
    if not re.fullmatch(r"fw-rsa3072:[0-9a-f]{64}", pin) or pin == bench_id():
        raise ImageError("release pin is invalid or the public bench key")
    reject_private_material(data)
    info = inspect_image(data, require_signed=True, require_manifest=True)
    if info.key_id != pin:
        raise ImageError("published block0 key does not match reviewed release pin")
    if len(data) >= MAX_SIGNED_BYTES:
        raise ImageError("signed image exceeds 2700 KiB budget")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bin", type=Path)
    p.add_argument("--sdkconfig-json", type=Path)
    p.add_argument("--elf", type=Path)
    p.add_argument("--pin", type=Path, default=RELEASE_PIN)
    p.add_argument("--boundary", type=Path, default=FIRST_SIGNED_RELEASE)
    p.add_argument("--intermediate", action="store_true", help="NOT a release check; padded build input only")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args(argv)
    if args.self_test:
        import subprocess
        return subprocess.call([sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tests"),
                                "-p", "test_ota_signing.py", "-v"], cwd=ROOT)
    try:
        if args.bin is None:
            p.error("--bin is required")
        if args.sdkconfig_json:
            check_config(json.loads(args.sdkconfig_json.read_text()))
        data = args.bin.read_bytes()
        if args.intermediate:
            if args.sdkconfig_json is None:
                raise ImageError("intermediate check requires effective sdkconfig")
            padded_intermediate(data)
            reject_private_material(data)
        else:
            load_boundary(args.boundary)
            check_release(data, load_pin(args.pin))
        if args.elf:
            reject_private_material(args.elf.read_bytes())
    except (ImageError, OSError, ValueError) as exc:
        print(f"SIGNING GUARD FAILED: {exc}", file=sys.stderr)
        return 1
    print("Intermediate verified (NOT bootable)" if args.intermediate else "Published signed image verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

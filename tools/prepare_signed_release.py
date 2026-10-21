#!/usr/bin/env python3
"""Sign/repack the PR intermediate before either release unit is published."""
from __future__ import annotations
import argparse
import io
import os
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from flash_gui.firmware_metadata import ImageError, inspect_image, key_id, sign_image
from tools.check_release_signing import RELEASE_PIN, check_release, load_pin, private_key, reject_private_material


def prepare(assets: Path, version: str, pem: str, pin_path: Path = RELEASE_PIN) -> None:
    pin = load_pin(pin_path)
    if not pem:
        raise ImageError("AMBYTE_FIRMWARE_SIGNING_KEY_PEM is absent; firmware and schedule publication stopped")
    key = private_key(pem.encode())
    if key_id(key) != pin:
        raise ImageError("CI private key does not match reviewed repository pin")
    raw_path = assets / "firmware.bin"
    unsigned = raw_path.read_bytes()
    info = inspect_image(unsigned, require_manifest=True)
    if info.version != version:
        raise ImageError("promoted app version differs from semantic-release preview")
    bundle_path = assets / f"ambyte-iot-v{version}.zip"
    old_bundle = bundle_path.read_bytes()
    with zipfile.ZipFile(io.BytesIO(old_bundle)) as bundle:
        names = bundle.namelist()
        if len(names) != len(set(names)) or names.count("ambyte-iot.bin") != 1:
            raise ImageError("ambiguous ZIP application")
        if bundle.read("ambyte-iot.bin") != unsigned:
            raise ImageError("PR raw and ZIP applications differ")
        for name in names:
            reject_private_material(bundle.read(name))
        signed = sign_image(unsigned, key)
        check_release(signed, pin)
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as new_bundle:
            for entry in bundle.infolist():
                new_bundle.writestr(entry, signed if entry.filename == "ambyte-iot.bin" else bundle.read(entry))
    # Validate the exact bytes to publish, before mutating either artifact.
    with zipfile.ZipFile(io.BytesIO(output.getvalue())) as bundle:
        if bundle.read("ambyte-iot.bin") != signed:
            raise ImageError("signed bundle/raw mismatch")
        check_release(bundle.read("ambyte-iot.bin"), pin)
    raw_path.write_bytes(signed)
    bundle_path.write_bytes(output.getvalue())


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--assets", type=Path, required=True)
    p.add_argument("--version", required=True)
    a = p.parse_args(argv)
    try:
        # Never write the secret to disk, subprocess arguments, logs or artifacts.
        prepare(a.assets, a.version, os.environ.pop("AMBYTE_FIRMWARE_SIGNING_KEY_PEM", ""))
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"Release signing failed; neither release unit published: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

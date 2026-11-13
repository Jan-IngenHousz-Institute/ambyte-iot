#!/usr/bin/env python3
"""Explicit physical bench flash, bench-signed (not OTA bootstrap).

Default: NVS is preserved (AMBYTE_NVS_SKIP=1), so an existing device keeps its
identity, certificate and Wi-Fi. --provision is for a board's FIRST flash: the
build generates nvs.bin from .env + device_certs/ and the upload writes it, which
replaces whatever identity the device had."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def flash_layout(build: Path, signed_app: Path, provision: bool):
    """Use IDF's actual addresses, including otadata selecting the flashed app.

    `pio -t nobuild -t upload` never loads the CMake upload metadata: it uses
    the board default 0x10000, even though this project's app starts at 0x20000.
    A successful esptool exit can therefore leave only an old OTA slot bootable.
    """
    manifest = json.loads((build / "flasher_args.json").read_text())
    app_offset = int(manifest["app"]["offset"], 0)
    offsets = {int(offset, 0) for offset in manifest["flash_files"]}
    if app_offset != 0x20000 or offsets != {0, 0x8000, 0xf000, 0x20000}:
        raise RuntimeError("Unsupported bench layout; no device was flashed")
    # PlatformIO writes these root-level artifacts. IDF's manifest names the
    # CMake paths instead; CI's flash ZIP packager makes the same mapping.
    images = [(0, build / "bootloader.bin"), (0x8000, build / "partitions.bin"),
              (0xf000, build / "ota_data_initial.bin"), (app_offset, signed_app)]
    from flash_gui.firmware_update import IMAGE_REGIONS
    for offset, path in images:
        size = path.stat().st_size
        if not size or offset + ((size + 4095) // 4096) * 4096 > IMAGE_REGIONS[offset]:
            raise RuntimeError("Bench image overlaps a preserved partition; no device was flashed")
    if provision:
        nvs = build / "nvs.bin"
        if nvs.stat().st_size != 0x6000:
            raise RuntimeError("Invalid provisioning NVS size; no device was flashed")
        images.append((0x9000, nvs))
    return sorted(images), manifest["flash_settings"]


def validate_signed_build(build: Path) -> None:
    """The PlatformIO before_upload guard, applied to this direct flash path.

    flash_images() bypasses PlatformIO's upload hook, which is where
    tools/upload_guard.validate_upload checks the EFFECTIVE sdkconfig (eFuse-
    burning options such as SECURE_DISABLE_ROM_DL_MODE, signed-update
    verification, rollback) and the signed image. Without this, a bench build
    with a dangerous cached setting could be bench-signed and flashed
    (gpt-6-astra review of #98). Runs after signing, before any port access.
    """
    sys.path.insert(0, str(ROOT))
    from tools.upload_guard import validate_upload
    try:
        validate_upload(build)
    except (ValueError, OSError) as exc:
        raise RuntimeError(f"UPLOAD REFUSED before esptool: {exc}; no device was flashed") from exc


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--env", default="esp32-s3-devkitm-1")
    p.add_argument("--port", required=True)
    p.add_argument("--expected-mac", default="", help="MAC to verify in the flash session; defaults to the selected USB port's MAC serial")
    p.add_argument("--provision", action="store_true",
                   help="first flash of a new board: also write NVS from .env + device_certs/ "
                        "(overwrites the device's identity, certificate and Wi-Fi)")
    a = p.parse_args(argv)
    from serial.tools import list_ports
    raw_mac = a.expected_mac or next((port.serial_number for port in list_ports.comports()
                                      if port.device == a.port), None) or ""
    digits = raw_mac.replace(":", "").upper()
    if not re.fullmatch(r"[0-9A-F]{12}", digits):
        p.error("selected port has no MAC serial; supply --expected-mac explicitly")
    expected_mac = ":".join(digits[index:index+2] for index in range(0,12,2))
    env = dict(os.environ)
    if a.provision:
        env.pop("AMBYTE_NVS_SKIP", None)   # extra_script.py builds and registers nvs.bin
    else:
        env["AMBYTE_NVS_SKIP"] = "1"
    image = ROOT / ".pio/build" / a.env / "firmware.bin"
    # A prior explicit signing run may have replaced the generated output.
    # Regenerate just that artifact, retaining the compiled ELF/object cache.
    image.unlink(missing_ok=True)
    if a.provision:
        # Never let a skipped build hook reuse another board's old credential
        # image. The current build must generate a fresh NVS image explicitly.
        (image.parent / "nvs.bin").unlink(missing_ok=True)
    subprocess.run(["pio", "run", "-e", a.env], cwd=ROOT, env=env, check=True)
    if a.provision and not (image.parent / "nvs.bin").is_file():
        raise RuntimeError("Provisioning build did not generate fresh NVS; no device was flashed")
    subprocess.run([sys.executable, str(ROOT / "tools/sign_firmware.py"), "--image", str(image),
                    "--key", str(ROOT / "tools/keys/UNSAFE_BENCH_ONLY_firmware.pem"),
                    "--allow-bench", "--output", str(image)], cwd=ROOT, check=True)
    validate_signed_build(image.parent)
    # Cross the same signature, MAC and partition-table checks as the GUI.
    # Partition/data layout changes are refused on an existing configured board.
    sys.path.insert(0, str(ROOT))
    from flash_gui import esptool_ops
    images, settings = flash_layout(image.parent, image, a.provision)
    # Do not reset the board in a separate read_mac session: native USB can
    # re-enumerate to another port between that reset and the actual flash.
    # The flash session verifies the selected USB MAC before any write.
    table = dict(images)[0x8000].read_bytes()
    esptool_ops.flash_images(a.port, images, settings, log=print,
                            expected_mac=expected_mac,
                            expected_partition_table=None if a.provision else table)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

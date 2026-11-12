#!/usr/bin/env python3
"""Explicit local signing. Release promotion uses prepare_signed_release.py."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from flash_gui.firmware_metadata import ImageError, key_id, sign_image
from tools.check_release_signing import bench_id, check_release, load_pin, private_key


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--key", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--allow-bench", action="store_true")
    a = p.parse_args(argv)
    try:
        key = private_key(a.key.read_bytes())
        signer = key_id(key)
        if signer == bench_id():
            if not a.allow_bench:
                raise ImageError("public bench key requires explicit --allow-bench")
            print("UNSAFE BENCH ONLY: anyone with this repository can sign for this image", file=sys.stderr)
        elif signer != load_pin():
            raise ImageError("signing key does not match reviewed release root")
        output = sign_image(a.image.read_bytes(), key)
        if signer != bench_id():
            check_release(output, load_pin())
        a.output.write_bytes(output)
        print(f"Signed {len(output)} bytes; key ID {signer}")
    except (ValueError, OSError) as exc:
        print(f"Signing refused: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

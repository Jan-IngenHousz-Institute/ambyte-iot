#!/usr/bin/env python3
"""Explicit physical bench provisioning, preserving NVS (not OTA bootstrap)."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--env", default="esp32-s3-devkitm-1")
    p.add_argument("--port", required=True)
    a = p.parse_args()
    env = {**os.environ, "AMBYTE_NVS_SKIP": "1"}
    image = ROOT / ".pio/build" / a.env / "firmware.bin"
    # A prior explicit signing run may have replaced the generated output.
    # Regenerate just that artifact, retaining the compiled ELF/object cache.
    image.unlink(missing_ok=True)
    subprocess.run(["pio", "run", "-e", a.env], cwd=ROOT, env=env, check=True)
    subprocess.run([sys.executable, str(ROOT / "tools/sign_firmware.py"), "--image", str(image),
                    "--key", str(ROOT / "tools/keys/UNSAFE_BENCH_ONLY_firmware.pem"),
                    "--allow-bench", "--output", str(image)], cwd=ROOT, check=True)
    subprocess.run(["pio", "run", "-e", a.env, "-t", "nobuild", "-t", "upload", "--upload-port", a.port],
                   cwd=ROOT, env=env, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

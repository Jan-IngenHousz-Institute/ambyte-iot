#!/usr/bin/env python3
"""Prepare all release assets, then publish affected units in order."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.prepare_signed_release import prepare


def publish_units(assets, firmware_version, schedule_release, pem, *, runner=subprocess.run):
    if firmware_version:
        prepare(assets, firmware_version, pem)
    if firmware_version:
        runner(["npx", "semantic-release"], cwd=ROOT, check=True)
    if schedule_release:
        runner([str(ROOT / "node_modules/.bin/semantic-release")], cwd=ROOT / "schedule", check=True)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--assets", type=Path, default=Path("release-assets"))
    p.add_argument("--firmware-version", default="")
    p.add_argument("--schedule-release", choices=("true", "false"), default="false")
    a = p.parse_args(argv)
    try:
        publish_units(a.assets, a.firmware_version, a.schedule_release == "true",
                      os.environ.pop("AMBYTE_FIRMWARE_SIGNING_KEY_PEM", ""))
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Release failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

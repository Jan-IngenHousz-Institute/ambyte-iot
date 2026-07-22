"""PlatformIO extra_script — pre-populate the NVS partition during firmware upload.

Registered in platformio.ini as `extra_scripts = pre:tools/extra_script.py`.

Flow per build:
  1. If AMBYTE_NVS_SKIP=1, do nothing (developer wants stock firmware).
  2. If .env is missing AND no AMBYTE_* env vars are set, do nothing — the
     project compiles without provisioning data (useful for size / lint checks).
  3. Otherwise invoke tools/build_nvs_image.py to write nvs.bin into the build
     dir, then register that binary with PlatformIO so `pio run -t upload`
     flashes it at offset 0x9000 alongside the firmware.

Failure mode: a half-populated .env causes build_nvs_image.py to exit non-zero
with the list of missing AMBYTE_* values — surfaces before upload, never silent.
"""

import os
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 — provided by PlatformIO/SCons

NVS_OFFSET = "0x9000"  # must match partitions.csv

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
BUILD_DIR   = Path(env.subst("$BUILD_DIR"))  # noqa: F821

DOTENV       = PROJECT_DIR / ".env"
BUILDER      = PROJECT_DIR / "tools" / "build_nvs_image.py"


def _truthy(value: str | None) -> bool:
    return (value or "").strip().lower() in ("1", "true", "yes", "on")


def _any_ambyte_env_set() -> bool:
    return any(k.startswith("AMBYTE_") for k in os.environ)


def _generate_nvs() -> Path | None:
    if _truthy(os.environ.get("AMBYTE_NVS_SKIP")):
        print("ambyte-nvs: AMBYTE_NVS_SKIP set — skipping NVS pre-population")
        return None

    if not DOTENV.exists() and not _any_ambyte_env_set():
        print("ambyte-nvs: no .env and no AMBYTE_* env vars — skipping NVS pre-population")
        return None

    if not BUILDER.is_file():
        print(f"ambyte-nvs: builder script missing at {BUILDER} — skipping", file=sys.stderr)
        return None

    nvs_bin = BUILD_DIR / "nvs.bin"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    print(f"ambyte-nvs: building NVS image -> {nvs_bin}")
    result = subprocess.run(
        [sys.executable, str(BUILDER), "--out", str(nvs_bin), "--quiet"],
        cwd=str(PROJECT_DIR),
    )
    if result.returncode != 0:
        print(f"ambyte-nvs: build_nvs_image.py failed (exit {result.returncode})",
              file=sys.stderr)
        env.Exit(result.returncode)  # noqa: F821
        return None  # unreachable

    return nvs_bin


_nvs_bin = _generate_nvs()
if _nvs_bin is not None:
    # FLASH_EXTRA_IMAGES is the espressif32-platform-recognised hook for
    # appending `<offset> <path>` pairs to the esptool write_flash invocation
    # used by `pio run -t upload`.
    env.Append(FLASH_EXTRA_IMAGES=[(NVS_OFFSET, str(_nvs_bin))])  # noqa: F821
    print(f"ambyte-nvs: registered {_nvs_bin.name} @ {NVS_OFFSET} for upload")

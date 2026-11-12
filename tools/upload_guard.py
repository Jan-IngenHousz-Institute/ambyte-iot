"""Shared by all PlatformIO environments, before any esptool flash write."""
from pathlib import Path
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from flash_gui.firmware_metadata import ImageError, inspect_image, check_config


def validate_upload(build_dir: Path) -> None:
    config = json.loads((build_dir / "config/sdkconfig.json").read_text())
    # Read effective config even for additive bench/HIL environments.
    check_config(config)
    inspect_image((build_dir / "firmware.bin").read_bytes(), require_signed=True, require_manifest=True)


def before_upload(source, target, env):
    try:
        validate_upload(Path(env.subst("$BUILD_DIR")))
    except (ValueError, OSError) as exc:
        raise RuntimeError(f"UPLOAD REFUSED before esptool: {exc}. Use the explicit signed bench/release path.") from exc

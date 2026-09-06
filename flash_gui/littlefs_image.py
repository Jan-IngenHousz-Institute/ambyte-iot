# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Bake a littlefs image of the `littlefs` partition with schedule.yaml inside.

Since the firmware keeps its schedule on internal flash (/littlefs/schedule.yaml), a
fresh board no longer needs the SD seed step: flashing this image at the
littlefs partition offset delivers the selected release script together with
the firmware, before first boot.

The image is built in memory with the pure-host `littlefs` package (bundles
littlefs 2.11 / disk format 2.1 — the exact version the firmware's vendored
esp_littlefs component uses), so no mklittlefs binary or ESP-IDF install is
needed. Geometry MUST match the firmware's sdkconfig (CONFIG_LITTLEFS_* and
the partition table): a mismatch mounts garbage on device.
"""

from __future__ import annotations

from pathlib import Path

from .config import LITTLEFS_PARTITION_SIZE

# Firmware values (sdkconfig.esp32-s3-devkitm-1 + partitions.csv):
_BLOCK_SIZE = 4096          # flash erase size; structural for the on-disk format
_READ_SIZE = 128            # CONFIG_LITTLEFS_READ_SIZE
_PROG_SIZE = 128            # CONFIG_LITTLEFS_WRITE_SIZE
_CACHE_SIZE = 512           # CONFIG_LITTLEFS_CACHE_SIZE
_LOOKAHEAD_SIZE = 128       # CONFIG_LITTLEFS_LOOKAHEAD_SIZE
_BLOCK_CYCLES = 512         # CONFIG_LITTLEFS_BLOCK_CYCLES
_NAME_MAX = 64              # CONFIG_LITTLEFS_OBJ_NAME_LEN

SCHEDULE_NAME = "schedule.yaml"
SCHEDULE_MAX_BYTES = 16 * 1024  # SCHED_YAML_MAX_FILE_BYTES in firmware


class LittlefsImageError(RuntimeError):
    """The image could not be built or did not survive a read-back check."""


def build_schedule_image(script: bytes, out_path: Path) -> Path:
    """Write a littlefs image containing /schedule.yaml with `script` as content.

    The rest of the partition is left erased (0xFF). The built image is
    verified by re-mounting it and comparing the file bytes before returning.
    """
    if not script:
        raise LittlefsImageError("refusing to bake an empty schedule.yaml")
    if len(script) > SCHEDULE_MAX_BYTES:
        raise LittlefsImageError(
            f"schedule.yaml exceeds the firmware's {SCHEDULE_MAX_BYTES}-byte limit"
        )
    from littlefs import LittleFS, UserContext   # lazy: host-only dependency

    ctx = UserContext(buffsize=LITTLEFS_PARTITION_SIZE)
    try:
        fs = LittleFS(context=ctx, mount=False,
                      block_size=_BLOCK_SIZE,
                      block_count=LITTLEFS_PARTITION_SIZE // _BLOCK_SIZE,
                      read_size=_READ_SIZE, prog_size=_PROG_SIZE,
                      cache_size=_CACHE_SIZE, lookahead_size=_LOOKAHEAD_SIZE,
                      block_cycles=_BLOCK_CYCLES, name_max=_NAME_MAX)
        fs.format()
        fs.mount()
        with fs.open(SCHEDULE_NAME, "wb") as f:
            f.write(script)
        fs.unmount()
    except Exception as exc:
        raise LittlefsImageError(f"littlefs image build failed: {exc}") from exc

    # Read-back: a corrupt image must fail HERE, not on the device.
    try:
        fs = LittleFS(context=UserContext(buffer=bytearray(ctx.buffer)),
                      mount=True,
                      block_size=_BLOCK_SIZE,
                      block_count=LITTLEFS_PARTITION_SIZE // _BLOCK_SIZE,
                      read_size=_READ_SIZE, prog_size=_PROG_SIZE,
                      cache_size=_CACHE_SIZE, lookahead_size=_LOOKAHEAD_SIZE,
                      block_cycles=_BLOCK_CYCLES, name_max=_NAME_MAX)
        with fs.open(SCHEDULE_NAME, "rb") as f:
            got = f.read()
        fs.unmount()
    except Exception as exc:
        raise LittlefsImageError(f"freshly built image does not mount: {exc}") from exc
    if got != script:
        raise LittlefsImageError("read-back of schedule.yaml differs from the scripted bytes")

    out_path = Path(out_path).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(ctx.buffer))
    return out_path

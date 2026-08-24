# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Packaged-runtime smoke checks used by the cross-platform build workflow."""

from __future__ import annotations

import tempfile
import traceback
from pathlib import Path

from .config import LITTLEFS_PARTITION_SIZE
from .littlefs_image import build_main_lua_image


REPORT_NAME = "packaged-littlefs-smoke.txt"


def run_littlefs_smoke(report_path: Path | None = None) -> None:
    """Build a real littlefs image and leave a CI-readable result report."""

    report_path = report_path or Path.cwd() / REPORT_NAME
    try:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "littlefs.bin"
            build_main_lua_image(b"return { packaged_smoke = true }\n", output)
            if output.stat().st_size != LITTLEFS_PARTITION_SIZE:
                raise RuntimeError(
                    f"littlefs smoke image is {output.stat().st_size} bytes, "
                    f"expected {LITTLEFS_PARTITION_SIZE}"
                )
    except Exception:
        report_path.write_text(traceback.format_exc(), encoding="utf-8")
        raise
    report_path.write_text(
        f"PASS: built and read back {LITTLEFS_PARTITION_SIZE}-byte littlefs image\n",
        encoding="utf-8",
    )

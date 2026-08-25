# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Packaged-runtime smoke checks used by the cross-platform build workflow."""

from __future__ import annotations

import hashlib
import tempfile
import traceback
from pathlib import Path

from . import release_fetch
from .config import LITTLEFS_PARTITION_SIZE
from .littlefs_image import build_main_lua_image
from .release_fetch import LuaScriptRelease


REPORT_NAME = "packaged-littlefs-smoke.txt"


def run_littlefs_smoke(report_path: Path | None = None) -> None:
    """Build a real littlefs image and leave a CI-readable result report."""

    report_path = report_path or Path.cwd() / REPORT_NAME
    try:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script_blob = b"return { packaged_smoke = true }\n"
            source = root / "released-main.lua"
            source.write_bytes(script_blob)
            digest = hashlib.sha256(script_blob).hexdigest()
            script = LuaScriptRelease(
                tag="lua-v0.0.0",
                asset_name="main.lua",
                script_name="main",
                script_version="0.0.0",
                built_against_fw="v0.0.0",
                asset_url=source.as_uri(),
                sha256=digest,
                size_bytes=len(script_blob),
                campaign_id="packaged-smoke",
            )

            # Exercise the same cold-cache/silent call that provisioning uses.
            # 0.2.9 passed None here, then script_bytes tried to call it as the
            # logger before the littlefs builder even started.
            previous_cache = release_fetch.SCRIPTS_CACHE_DIR
            release_fetch.SCRIPTS_CACHE_DIR = root / "script-cache"
            try:
                fetched_blob = release_fetch.script_bytes(script, log=None)
            finally:
                release_fetch.SCRIPTS_CACHE_DIR = previous_cache

            output = Path(directory) / "littlefs.bin"
            build_main_lua_image(fetched_blob, output)
            if output.stat().st_size != LITTLEFS_PARTITION_SIZE:
                raise RuntimeError(
                    f"littlefs smoke image is {output.stat().st_size} bytes, "
                    f"expected {LITTLEFS_PARTITION_SIZE}"
                )
    except Exception:
        report_path.write_text(traceback.format_exc(), encoding="utf-8")
        raise
    report_path.write_text(
        "PASS: cold-cache fetch, digest check, and "
        f"{LITTLEFS_PARTITION_SIZE}-byte littlefs image read-back\n",
        encoding="utf-8",
    )

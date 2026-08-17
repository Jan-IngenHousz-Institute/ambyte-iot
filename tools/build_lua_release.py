#!/usr/bin/env python3
"""Build one immutable Lua asset and its rollout manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?$")
ASSET_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*\.lua$")


def build(
    source: Path,
    output_dir: Path,
    version: str,
    built_against_fw: str,
    repository: str,
    asset_name: str = "main.lua",
) -> dict:
    if not SEMVER.fullmatch(version):
        raise ValueError(f"invalid Lua semantic version: {version}")
    if not built_against_fw:
        raise ValueError("built-against firmware version is required")
    if ASSET_NAME.fullmatch(asset_name) is None:
        raise ValueError(f"invalid Lua asset name: {asset_name!r}")

    body = source.read_bytes()
    digest = hashlib.sha256(body).hexdigest()
    tag = f"lua-v{version}"
    script_name = asset_name.removesuffix(".lua")
    campaign_id = tag if script_name == "main" else f"{tag}:{script_name}"
    repository = repository.rstrip("/")
    asset_url = f"https://github.com/{repository}/releases/download/{tag}/{asset_name}"

    output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output_dir / asset_name)
    manifest = {
        "schema_version": 1,
        "script_name": script_name,
        "script_version": version,
        "tag": tag,
        "sha256": digest,
        "size_bytes": len(body),
        "built_against_fw": built_against_fw,
        "asset_url": asset_url,
        "script_update": {
            "type": "script_update",
            "id": campaign_id,
            "url": asset_url,
            "checksum": digest,
            "script_version": version,
            "built_against_fw": built_against_fw,
        },
    }
    (output_dir / f"{asset_name}.manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("lua/main.lua"))
    parser.add_argument("--asset-name", default="main.lua")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--built-against-fw", required=True)
    parser.add_argument("--repository", required=True)
    args = parser.parse_args()
    manifest = build(
        args.source,
        args.output_dir,
        args.version,
        args.built_against_fw,
        args.repository,
        args.asset_name,
    )
    print(
        f"Lua {manifest['script_name']} {manifest['script_version']}: "
        f"{manifest['size_bytes']} bytes, "
        f"sha256={manifest['sha256']}"
    )


if __name__ == "__main__":
    main()

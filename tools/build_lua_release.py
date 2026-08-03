#!/usr/bin/env python3
"""Build the immutable main.lua asset and its rollout manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?$")


def build(source: Path, output_dir: Path, version: str, built_against_fw: str, repository: str) -> dict:
    if not SEMVER.fullmatch(version):
        raise ValueError(f"invalid Lua semantic version: {version}")
    if not built_against_fw:
        raise ValueError("built-against firmware version is required")

    body = source.read_bytes()
    digest = hashlib.sha256(body).hexdigest()
    tag = f"lua-v{version}"
    repository = repository.rstrip("/")
    asset_url = f"https://github.com/{repository}/releases/download/{tag}/main.lua"

    output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output_dir / "main.lua")
    manifest = {
        "schema_version": 1,
        "script_version": version,
        "tag": tag,
        "sha256": digest,
        "size_bytes": len(body),
        "built_against_fw": built_against_fw,
        "asset_url": asset_url,
        "script_update": {
            "type": "script_update",
            "id": tag,
            "url": asset_url,
            "checksum": digest,
            "script_version": version,
            "built_against_fw": built_against_fw,
        },
    }
    (output_dir / "main.lua.manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("lua/main.lua"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--built-against-fw", required=True)
    parser.add_argument("--repository", required=True)
    args = parser.parse_args()
    manifest = build(
        args.source, args.output_dir, args.version, args.built_against_fw, args.repository
    )
    print(
        f"Lua {manifest['script_version']}: {manifest['size_bytes']} bytes, "
        f"sha256={manifest['sha256']}"
    )


if __name__ == "__main__":
    main()

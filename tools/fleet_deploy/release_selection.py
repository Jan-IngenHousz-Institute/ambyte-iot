#!/usr/bin/env python3
"""Validate fleet release tags and select the highest stable semver by kind."""

from __future__ import annotations

import argparse
from functools import cmp_to_key
import json
import re
import sys
from typing import Any

try:
    from . import fleet_deploy as fleet
except ImportError:  # pragma: no cover - direct CLI invocation
    import fleet_deploy as fleet


SEMVER_SUFFIX = (
    r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)
FIRMWARE_TAG_RE = re.compile(rf"^v{SEMVER_SUFFIX}$")
LUA_TAG_RE = re.compile(rf"^lua-v{SEMVER_SUFFIX}$")
TAG_PATTERNS = {"ota": FIRMWARE_TAG_RE, "lua": LUA_TAG_RE}


def parse_tag(tag: str, kind: str):
    pattern = TAG_PATTERNS[kind]
    if pattern.fullmatch(tag) is None:
        raise ValueError(f"{tag!r} is not a valid {kind} release tag")
    prefix = "lua-v" if kind == "lua" else "v"
    parsed = fleet.parse_version(tag.removeprefix(prefix))
    if parsed is None:  # defensive: regex and fleet semver parser must agree
        raise ValueError(f"{tag!r} has no parseable semantic version")
    return parsed


def select_latest(releases: Any, kind: str) -> str:
    if not isinstance(releases, list):
        raise ValueError("GitHub release response must be a JSON array")
    candidates = []
    for release in releases:
        if not isinstance(release, dict):
            continue
        if release.get("isDraft") or release.get("isPrerelease"):
            continue
        tag = release.get("tagName")
        if not isinstance(tag, str):
            continue
        try:
            version = parse_tag(tag, kind)
        except ValueError:
            continue
        if version[1]:  # Never let a mislabeled prerelease become `latest`.
            continue
        candidates.append((version, tag))
    if not candidates:
        raise ValueError(f"no published stable {kind} release was found")
    candidates.sort(key=cmp_to_key(lambda a, b: fleet.cmp_version(a[0], b[0])))
    return candidates[-1][1]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=sorted(TAG_PATTERNS), required=True)
    parser.add_argument("--tag", help="validate and echo an exact tag")
    args = parser.parse_args(argv)
    try:
        if args.tag is not None:
            parse_tag(args.tag, args.kind)
            selected = args.tag
        else:
            selected = select_latest(json.load(sys.stdin), args.kind)
    except (ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    print(selected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

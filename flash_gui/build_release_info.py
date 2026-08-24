# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Generate the release identity embedded in packaged flash GUI builds."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from .app_update import GUI_TAG_RE


def payload(tag: str, source_sha: str) -> dict[str, object]:
    if GUI_TAG_RE.fullmatch(tag) is None:
        raise ValueError(f"invalid GUI release tag: {tag!r}")
    if re.fullmatch(r"[0-9a-f]{40}", source_sha) is None:
        raise ValueError("source SHA must be 40 lowercase hexadecimal characters")
    return {"schema_version": 1, "tag": tag, "source_sha": source_sha}


def write_release_info(path: Path, tag: str, source_sha: str) -> None:
    path.write_text(
        json.dumps(payload(tag, source_sha), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    write_release_info(args.output, args.tag, args.source_sha)


if __name__ == "__main__":
    main()

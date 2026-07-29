#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.firmware_candidate.candidate import CandidateError, build_and_package


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and package one exact unprovisioned firmware candidate")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--pio", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--release-notes", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        output = build_and_package(
            repo_root=args.repo_root.resolve(),
            pio=args.pio.resolve(),
            metadata_path=args.metadata.resolve(),
            release_notes=args.release_notes.resolve(),
            output_dir=args.output_dir.resolve(),
            firmware_version=args.version,
        )
    except CandidateError as exc:
        print(f"firmware-candidate: {exc}", file=sys.stderr)
        return 2
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

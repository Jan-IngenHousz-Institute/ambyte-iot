#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.firmware_candidate.candidate import CandidateError, verify_candidate


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify an Ambyte firmware candidate and its inner bundle")
    parser.add_argument("candidate_dir", type=Path)
    args = parser.parse_args()
    try:
        manifest = verify_candidate(args.candidate_dir)
    except (CandidateError, OSError) as exc:
        print(f"firmware-candidate: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"ok": True, "artifact_name": manifest["artifact_name"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

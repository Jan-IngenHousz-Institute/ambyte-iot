#!/usr/bin/env python3
"""C5/J2: pre-existing suite regression gate.

  compare_suites.py base-collect.txt branch-collect.txt base.xml branch.xml

Fails if (1) any test id collected on the baseline is not collected on the
branch, (2) any branch failure/error id is not also a failure/error on the
baseline, or (3) any id the baseline ran is missing from the branch run.
Ids the branch ADDS (new files or new tests) are reported, never excluded
from the failure check. (It used to exclude every tests/test_evq_* id: right
for the PR that added those files, wrong ever after - pre-existing test_evq_*
ids then looked "missing" and their failures were ignored.)
"""
from __future__ import annotations

import sys
import xml.etree.ElementTree as ET


def norm_collect(line: str) -> str:
    # tests/test_x.py::Class::test_y[p]  ->  tests.test_x.Class::test_y[p]
    path, _, rest = line.partition(".py::")
    parts = rest.split("::")
    return ".".join([path.replace("/", ".")] + parts[:-1]) + "::" + parts[-1]


def collected(p: str) -> set[str]:
    ids = set()
    for line in open(p):
        line = line.strip()
        if ".py::" in line and not line.startswith(("=", "-")):
            ids.add(norm_collect(line))
    return ids


def outcomes(p: str) -> tuple[set[str], set[str]]:
    ran, bad = set(), set()
    for tc in ET.parse(p).getroot().iter("testcase"):
        tid = f"{tc.get('classname', '')}::{tc.get('name', '')}"
        ran.add(tid)
        if tc.find("failure") is not None or tc.find("error") is not None:
            bad.add(tid)
    return ran, bad


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    bc, rc, bx, rx = sys.argv[1:5]
    base_ids = collected(bc)
    branch_ids = collected(rc)
    rc_ = 0
    gone = sorted(base_ids - branch_ids)
    added = sorted(branch_ids - base_ids)
    print(f"collected: baseline {len(base_ids)}, branch {len(branch_ids)} ({len(added)} added)")
    if not base_ids:
        print("baseline collection is empty")
        rc_ = 1
    if gone:
        print(f"pre-existing ids no longer collected on branch: {gone[:20]}")
        rc_ = 1
    base_ran, base_bad = outcomes(bx)
    br_ran, br_bad = outcomes(rx)
    new_bad = sorted(br_bad - base_bad)
    missing = sorted(base_ran - br_ran)
    print(f"baseline: {len(base_ran)} ran, {len(base_bad)} failed/errored; branch: {len(br_ran)} ran, {len(br_bad)} failed/errored")
    if new_bad:
        print(f"NEW failures on branch: {new_bad[:20]}")
        rc_ = 1
    if missing:
        print(f"pre-existing ids missing on branch: {missing[:20]}")
        rc_ = 1
    fixed = sorted(base_bad - br_bad)
    if fixed:
        print(f"failing on baseline but passing on branch: {len(fixed)}")
    print("OK" if rc_ == 0 else "FAIL")
    return rc_


if __name__ == "__main__":
    sys.exit(main())

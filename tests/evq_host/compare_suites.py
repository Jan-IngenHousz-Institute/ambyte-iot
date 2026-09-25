#!/usr/bin/env python3
"""C5/J2: pre-existing suite regression gate.

  compare_suites.py base-collect.txt branch-collect.txt base.xml branch.xml

Fails if (1) the pre-existing test-id collections differ (ids from test files
the branch ADDS, tests/test_evq_*, are excluded from the branch side), (2) any
branch failure/error id is not also a failure/error on baseline, or (3) any
pre-existing id is missing from the branch run.
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
    bc, rc, bx, rx = sys.argv[1:5]
    base_ids = collected(bc)
    branch_ids = {i for i in collected(rc) if not i.startswith("tests.test_evq_")}
    rc_ = 0
    if base_ids != branch_ids:
        print(f"collection differs: only-base={sorted(base_ids - branch_ids)[:10]} only-branch={sorted(branch_ids - base_ids)[:10]}")
        rc_ = 1
    base_ran, base_bad = outcomes(bx)
    br_ran, br_bad = outcomes(rx)
    br_bad = {i for i in br_bad if "test_evq_" not in i}
    new_bad = sorted(br_bad - base_bad)
    missing = sorted(i for i in base_ran if i not in br_ran and "test_evq_" not in i)
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

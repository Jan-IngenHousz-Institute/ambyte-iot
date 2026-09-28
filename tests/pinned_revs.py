"""Resolve the Sprint 1/2 base commits that some host tests diff against.

Those tests were written against the short ids of the commits they were proven
on (4ec9cef = Sprint 1 settled fix, 6fcbeba = C2). The PR branch carries the same
trees re-signed for push, under new ids, so a bare `git rev-parse 4ec9cef` fails
in CI although the content is byte-identical. Resolve the recorded id first, then
fall back to the unique commit in HEAD's history with the exact recorded subject;
anything else is an error, never a silent skip - a test that quietly diffs
against the wrong base would pass vacuously.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (recorded short id, exact subject, recorded tree) - the tree is what the tests were proven against.
SPRINT1_SETTLED = ("4ec9cef", "fix(storage): harden SD writers against write-induced corruption",
                   "7c7e4654101e52cc08c4a9e13f333896e6b00c0b")
C2 = ("6fcbeba", "test(hil): add sd_logger byte-exact trace, two-slot fault injector and USB qualification checkers",
      "61da1f254653819d330b84c9cf7f61a9d135d6c3")


def _git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)


def _tree(rev: str) -> str:
    return _git("rev-parse", f"{rev}^{{tree}}").stdout.strip()


def resolve(pin: tuple[str, str, str]) -> str:
    short, subject, tree = pin
    r = _git("rev-parse", "--verify", "--quiet", f"{short}^{{commit}}")
    if r.returncode == 0 and r.stdout.strip():
        rev = r.stdout.strip()
    else:
        r = _git("log", "--format=%H%x00%s", "HEAD")
        hits = [ln.split("\0", 1)[0] for ln in r.stdout.splitlines() if ln.split("\0", 1)[-1] == subject]
        if len(hits) != 1:
            raise RuntimeError(f"cannot resolve pinned base {short} ({subject!r}): {len(hits)} subject matches in HEAD history")
        rev = hits[0]
    got = _tree(rev)
    if got != tree:
        raise RuntimeError(f"pinned base {short} resolved to {rev[:12]} whose tree {got[:12]} != recorded {tree[:12]}")
    return rev

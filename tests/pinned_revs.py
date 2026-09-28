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

SPRINT1_SETTLED = ("4ec9cef", "fix(storage): harden SD writers against write-induced corruption")
C2 = ("6fcbeba", "test(hil): add sd_logger byte-exact trace, two-slot fault injector and USB qualification checkers")


def _git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)


def resolve(pin: tuple[str, str]) -> str:
    short, subject = pin
    r = _git("rev-parse", "--verify", "--quiet", f"{short}^{{commit}}")
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip()
    r = _git("log", "--format=%H%x00%s", "HEAD")
    hits = [ln.split("\0", 1)[0] for ln in r.stdout.splitlines() if ln.split("\0", 1)[-1] == subject]
    if len(hits) != 1:
        raise RuntimeError(f"cannot resolve pinned base {short} ({subject!r}): {len(hits)} subject matches in HEAD history")
    return hits[0]

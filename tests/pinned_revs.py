"""Resolve the Sprint 1/2 base commits that some host tests diff against.

Those tests were written against the short ids of the commits they were proven
on (4ec9cef = Sprint 1 settled fix, 6fcbeba = C2). The PR branch carries the same
trees re-signed for push, under new ids, so a bare `git rev-parse 4ec9cef` fails
in CI although the content is byte-identical. Resolve the recorded id first, then
fall back to a commit on any ref carrying both the exact recorded subject AND the
recorded tree; anything else is an error, never a silent skip - a test that
quietly diffs against the wrong base would pass vacuously.

The tree, not the subject, is the proof of identity. pr.yml presents the PR to
semantic-release as ONE squash commit titled with the PR title, and PR #75's title
is exactly 4ec9cef's subject: a subject-only lookup in HEAD's history resolved the
Sprint 1 base to that squash commit - the whole PR tree - and would have diffed
the PR against itself (CI run 36496875025 caught it via the tree check). HEAD
alone is also not enough: after the squash the PR's own commits are reachable only
from the remote branch ref, hence --all.
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


def _git(*args: str, root: Path = ROOT) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True)


def _tree(rev: str, root: Path = ROOT) -> str:
    return _git("rev-parse", f"{rev}^{{tree}}", root=root).stdout.strip()


def resolve(pin: tuple[str, str, str], root: Path = ROOT) -> str:
    short, subject, tree = pin
    r = _git("rev-parse", "--verify", "--quiet", f"{short}^{{commit}}", root=root)
    if r.returncode == 0 and r.stdout.strip():
        rev = r.stdout.strip()
        got = _tree(rev, root)
        if got != tree:     # the recorded id itself naming other content is an error, not a fallback
            raise RuntimeError(f"pinned base {short} resolved to {rev[:12]} whose tree {got[:12]} != recorded {tree[:12]}")
        return rev
    r = _git("log", "--all", "--format=%H%x00%T%x00%s", root=root)
    rows = [ln.split("\0", 2) for ln in r.stdout.splitlines()]
    same_subject = [row for row in rows if len(row) == 3 and row[2] == subject]
    hits = [h for h, t, _s in same_subject if t == tree]
    if not hits:
        raise RuntimeError(f"cannot resolve pinned base {short} ({subject!r}): {len(same_subject)} commit(s) on any ref "
                           f"carry the subject, none with recorded tree {tree[:12]}")
    return hits[0]      # every hit has the recorded tree: content-identical, any one is the base

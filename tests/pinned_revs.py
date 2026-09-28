"""Self-contained Sprint 1 / C2 baselines that some host tests diff or build against.

Those tests were proven against two commits of the SD write-integrity branch:
4ec9cef (Sprint 1 settled fix) and 6fcbeba (C2). Neither survives into main - the
branch was re-signed for push (3f0ab02d / 93ab22f7, byte-identical trees) and main
takes PRs as ONE squash commit, after which the branch commits are on no ref at all.
Looking them up at test time cannot work, and every workaround tried failed in a way
worth remembering:
  - a bare `git rev-parse 4ec9cef` fails in CI (the ids were never pushed);
  - a subject lookup in HEAD's history matched pr.yml's own squash commit, which is
    titled with the PR title == 4ec9cef's subject, i.e. the WHOLE PR tree: CI run
    36496730845 diffed the PR against itself and test_iso_names /
    test_sd_write_integrity passed vacuously (run 36496875025's tree guard caught it);
  - `git log --all` works only while a branch or tag keeps the commits alive - a
    runtime prerequisite on a ref is exactly what must not exist.

So each baseline is committed as a fixture: a patch (tests/fixtures/pinned_base/)
holding ONLY the changes to the paths its consumers read, applied to MAIN_BASE
(e1ca6ee, the #72 squash commit on main - immutable, and an ancestor of every later
main commit). resolve() rebuilds the baseline tree in a private temporary object
directory (the repository's own objects are a read-only alternate; nothing is
written to the repo) and refuses it unless
  1. the patch bytes hash to the recorded sha256, and
  2. the rebuilt tree's (mode, blob, path) listing of every consumed path hashes to
     the digest recorded from the REAL tree (7c7e4654... / 61da1f25...).
(2) is the proof of identity: changing any consumed baseline byte - in the patch or
anywhere else - changes a blob id and the digest. Paths outside `consumed` keep
e1ca6ee's content in the rebuilt tree, which is why its tree id differs from the
recorded one and why consumers must not read outside `consumed`.

Consumers run git against the returned tree id with env() (it carries the temporary
object directory). `python tests/pinned_revs.py --regenerate` rebuilds the fixtures
and prints the constants; it needs the real commits and refuses a tree mismatch.
"""
from __future__ import annotations

import atexit
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "fixtures" / "pinned_base"

# fix(storage): preserve unsent measurements with durable SD overflow (#72) - on main.
MAIN_BASE = "e1ca6ee9bb1cecb497014d810966bd4dc4410279"
# v1.11.0 release commit (G-MIG's "old" side) - on main; pinned by id, not by tag name.
V1_11_0 = "8af0c76aed5b53744783eb4932a5a275b7d52a99"


def _sprint1_consumed(p: str) -> bool:
    # test_iso_names: `git diff <base> -- *.c *.h` and `git grep <base> -- components main`;
    # test_sd_write_integrity FIO-2: `git show <base>:components/event_log/evq_hil_{trace,io}.c`.
    return p.endswith((".c", ".h")) or p.startswith(("components/", "main/"))


_GMIG_FILES = {"components/sd_card/sd_card.h", "components/sd_card/sd_diag.h", "components/sd_card/sd_diag_core.c",
               "tests/evq_host/stubs.c", "tests/evq_host/fsshim.c", "tests/evq_host/fsshim.h",
               "tests/evq_host/sha256.c", "tests/evq_host/sha256.h", "tests/evq_host/evq_host.h"}


def _c2_consumed(p: str) -> bool:
    # tests/gmig_host/build.py _new_files(): ls-tree + show of these (a superset is harmless).
    return p.startswith(("components/event_log/", "components/domain/include/", "tests/evq_host/stubs/")) \
        or p in _GMIG_FILES


@dataclass(frozen=True)
class Pin:
    name: str
    original: str           # the commit the tests were proven on (local only, never pushed)
    resigned: str           # its re-signed twin that was pushed on the PR branch (same tree)
    tree: str               # recorded tree of both
    parent: "Pin | None"    # the patch applies to the parent's rebuilt tree; None = MAIN_BASE
    consumed: Callable[[str], bool]
    patch: str              # fixture file under FIXTURES
    patch_sha256: str
    digest: str             # sha256 of the consumed (mode, blob, path) listing of `tree`


SPRINT1_SETTLED = Pin(
    "sprint1", "4ec9cef367db8181c422b2f7eeeccc504b7eb237", "3f0ab02d7d2ab45638689f81503fd5fb57bce275",
    "7c7e4654101e52cc08c4a9e13f333896e6b00c0b", None, _sprint1_consumed, "sprint1-4ec9cef.patch",
    "3e2e5a765dd4ab0d80e61f9573ceee2f126c5b5e3ca01bf31dc7d6af5f1d50ba",
    "97553fd9a1f1a02efa8d790a054f49eef02592431b964e29aeffc0447cdddde0")
C2 = Pin(
    "c2", "6fcbebaa1ad1560b5e4b00a4d08b0bee2e821bf6", "93ab22f7b03098aa0fbba46c1867f27444f7dfaf",
    "61da1f254653819d330b84c9cf7f61a9d135d6c3", SPRINT1_SETTLED, _c2_consumed, "c2-6fcbeba.patch",
    "f758738f69b85c80e2a58a47937cf845e9abb2f298b84038942561e2bcd3c1b2",
    "6090a4b2d8d4ffe407334d4cfdc4a7bcede54e2eb6465566d5ecebc8d46c6f2a")
PINS = (SPRINT1_SETTLED, C2)

_objdir: Path | None = None
_trees: dict[str, str] = {}


def env() -> dict[str, str]:
    """Environment for git commands that read a resolved baseline tree."""
    global _objdir
    if _objdir is None:
        tmp = Path(tempfile.mkdtemp(prefix="pinned_base-"))
        atexit.register(shutil.rmtree, tmp, True)
        (tmp / "objects").mkdir()
        _objdir = tmp / "objects"
    common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=ROOT,
                            capture_output=True, text=True, check=True).stdout.strip()
    e = {k: v for k, v in os.environ.items() if k not in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE")}
    e["GIT_OBJECT_DIRECTORY"] = str(_objdir)
    e["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = str(Path(common) / "objects")
    return e


def _git(*args: str, e: dict[str, str] | None = None, check: bool = True) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, env=e or env())
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout


def listing_digest(tree: str, consumed: Callable[[str], bool]) -> str:
    rows = [ln for ln in _git("ls-tree", "-r", "--full-tree", tree).splitlines() if consumed(ln.split("\t", 1)[1])]
    return hashlib.sha256("\n".join(rows).encode()).hexdigest()


def resolve(pin: Pin, fixtures: Path = FIXTURES) -> str:
    """Rebuild `pin`'s consumed baseline from its committed fixture; returns a tree id for env()."""
    key = f"{pin.name}:{fixtures}"
    if key in _trees:
        return _trees[key]
    if pin.parent is None:
        if _git("cat-file", "-t", MAIN_BASE, check=False).strip() != "commit":
            raise RuntimeError(f"main base {MAIN_BASE[:12]} is not in this clone (shallow checkout?)")
        base = MAIN_BASE
    else:
        base = resolve(pin.parent, fixtures)
    patch = fixtures / pin.patch
    got = hashlib.sha256(patch.read_bytes()).hexdigest()
    if got != pin.patch_sha256:
        raise RuntimeError(f"pinned base {pin.name}: {patch.name} sha256 {got[:12]} != recorded {pin.patch_sha256[:12]}")
    with tempfile.TemporaryDirectory() as t:
        e = {**env(), "GIT_INDEX_FILE": str(Path(t) / "index")}
        _git("read-tree", base, e=e)
        _git("apply", "--cached", "--binary", str(patch), e=e)
        tree = _git("write-tree", e=e).strip()
    got = listing_digest(tree, pin.consumed)
    if got != pin.digest:
        raise RuntimeError(f"pinned base {pin.name}: rebuilt consumed listing {got[:12]} != recorded {pin.digest[:12]} "
                           f"(tree {pin.tree[:12]}) - baseline bytes changed")
    _trees[key] = tree
    return tree


def _regenerate() -> None:
    """Rebuild the fixtures from the real commits (either twin) and print the constants."""
    FIXTURES.mkdir(parents=True, exist_ok=True)
    real: dict[str, str] = {}
    for pin in PINS:
        rev = next((c for c in (pin.original, pin.resigned)
                    if _git("cat-file", "-t", c, check=False).strip() == "commit"), None)
        if rev is None or _git("rev-parse", f"{rev}^{{tree}}").strip() != pin.tree:
            raise SystemExit(f"{pin.name}: neither {pin.original[:9]} nor {pin.resigned[:9]} with tree {pin.tree[:12]} here")
        real[pin.name] = rev
        prev = real[pin.parent.name] if pin.parent else MAIN_BASE
        if pin.parent is not None:      # the parent's rebuilt tree must equal its real tree where this pin reads
            if listing_digest(resolve(pin.parent), pin.consumed) != listing_digest(prev, pin.consumed):
                raise SystemExit(f"{pin.name}: {pin.parent.name} fixture does not cover every path {pin.name} reads")
        paths = [p for p in _git("diff", "--name-only", "--no-renames", prev, rev).split("\n") if p and pin.consumed(p)]
        data = subprocess.run(["git", "diff", "--binary", "--full-index", "--no-renames", prev, rev, "--", *paths],
                              cwd=ROOT, capture_output=True, check=True, env=env()).stdout
        (FIXTURES / pin.patch).write_bytes(data)
        print(f"{pin.name}: {len(paths)} paths, {len(data)} B  patch_sha256={hashlib.sha256(data).hexdigest()}  "
              f"digest={listing_digest(pin.tree, pin.consumed)}")


if __name__ == "__main__":
    if sys.argv[1:] == ["--regenerate"]:
        _regenerate()
    else:
        raise SystemExit("usage: python tests/pinned_revs.py --regenerate")

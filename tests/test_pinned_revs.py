"""pinned_revs: the Sprint 1 / C2 baselines rebuild from their committed fixtures on
top of main alone (no branch, tag, reflog or dangling object), match the recorded
trees on every consumed path, and any changed baseline byte is refused."""
from __future__ import annotations

import dataclasses
import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pinned_revs as P  # noqa: E402


def _mutate_added_line(patch: Path) -> None:
    """Change one byte of baseline CONTENT (an added line), keeping the patch applicable."""
    lines = patch.read_bytes().split(b"\n")
    i = next(i for i, ln in enumerate(lines) if ln.startswith(b"+") and not ln.startswith(b"+++") and len(ln) > 8)
    lines[i] = lines[i][:-1] + (b"#" if lines[i][-1:] != b"#" else b"@")
    patch.write_bytes(b"\n".join(lines))


class PinnedBaselines(unittest.TestCase):
    def test_pins_rebuild_to_recorded_consumed_listing(self):
        for pin in P.PINS:
            tree = P.resolve(pin)
            self.assertEqual(P.listing_digest(tree, pin.consumed), pin.digest, pin.name)

    def test_rebuilt_equals_real_commit_where_consumed(self):
        """Independent of the recorded digest: against the real tree, when this clone has it."""
        for pin in P.PINS:
            real = next((c for c in (pin.original, pin.resigned)
                         if P._git("cat-file", "-t", c, check=False).strip() == "commit"), None)
            if real is None:
                self.skipTest(f"{pin.name}: real commit not in this clone (expected after the squash merge)")
            self.assertEqual(P._git("rev-parse", f"{real}^{{tree}}").strip(), pin.tree)
            self.assertEqual(P.listing_digest(P.resolve(pin), pin.consumed), P.listing_digest(real, pin.consumed))

    def test_main_bases_are_on_head_history(self):
        for rev in (P.MAIN_BASE, P.V1_11_0):
            r = subprocess.run(["git", "merge-base", "--is-ancestor", rev, "HEAD"], cwd=P.ROOT)
            self.assertEqual(r.returncode, 0, f"{rev[:12]} is not an ancestor of HEAD")

    def test_rebuild_writes_nothing_into_the_repository(self):
        tree = P.resolve(P.SPRINT1_SETTLED)
        r = subprocess.run(["git", "cat-file", "-e", tree], cwd=P.ROOT, capture_output=True)
        self.assertNotEqual(r.returncode, 0, "the rebuilt tree leaked into the repository object store")

    def test_changed_patch_bytes_are_refused(self):
        for pin in P.PINS:
            with tempfile.TemporaryDirectory() as d:
                fx = Path(d) / "fx"
                shutil.copytree(P.FIXTURES, fx)
                _mutate_added_line(fx / pin.patch)
                with self.assertRaisesRegex(RuntimeError, "sha256"):        # the recorded patch hash
                    P.resolve(pin, fx)

    def test_changed_baseline_content_is_refused_even_with_a_matching_patch_hash(self):
        """The digest from the real tree, not the patch hash, is the identity proof."""
        for pin in P.PINS:
            with tempfile.TemporaryDirectory() as d:
                fx = Path(d) / "fx"
                shutil.copytree(P.FIXTURES, fx)
                _mutate_added_line(fx / pin.patch)
                forged = dataclasses.replace(pin, patch_sha256=hashlib.sha256((fx / pin.patch).read_bytes()).hexdigest())
                with self.assertRaisesRegex(RuntimeError, "baseline bytes changed"):
                    P.resolve(forged, fx)

    def test_c2_refuses_a_changed_sprint1_fixture(self):
        with tempfile.TemporaryDirectory() as d:
            fx = Path(d) / "fx"
            shutil.copytree(P.FIXTURES, fx)
            _mutate_added_line(fx / P.SPRINT1_SETTLED.patch)
            with self.assertRaises(RuntimeError):
                P.resolve(P.C2, fx)


if __name__ == "__main__":
    unittest.main()

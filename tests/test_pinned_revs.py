"""pinned_revs: the Sprint 1/2 base commits resolve to content-identical trees
(original or re-signed twin), and a subject match with a different tree is refused -
including pr.yml's squash commit, which carries the PR title as its subject."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pinned_revs  # noqa: E402


class PinnedRevs(unittest.TestCase):
    def test_pins_resolve_to_recorded_trees(self):
        for pin in (pinned_revs.SPRINT1_SETTLED, pinned_revs.C2):
            rev = pinned_revs.resolve(pin)
            self.assertEqual(pinned_revs._tree(rev), pin[2], pin[0])

    def test_tree_mismatch_is_refused(self):
        short, subject, _tree = pinned_revs.SPRINT1_SETTLED
        with self.assertRaises(RuntimeError):
            pinned_revs.resolve((short, subject, "0" * 40))

    def test_unknown_subject_is_refused(self):
        with self.assertRaises(RuntimeError):
            pinned_revs.resolve(("0000000", "no such subject in history", "0" * 40))


    def test_pr_squash_commit_with_same_subject_is_not_the_base(self):
        """Replays pr.yml:69-76: the PR is soft-reset onto its merge base and re-committed
        as one commit titled with the PR title (= the base's subject). The base must still
        resolve to the real commit, reachable only from the PR branch ref."""
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            env = ["-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]

            def git(*a):
                return subprocess.run(["git", *env, *a], cwd=root, check=True, capture_output=True, text=True).stdout.strip()

            git("init", "-q", "-b", "main")
            (root / "f").write_text("main\n"); git("add", "f"); git("commit", "-q", "-m", "chore: main")
            base_sha = git("rev-parse", "HEAD")
            git("checkout", "-q", "-b", "pr")
            (root / "f").write_text("sprint1\n"); git("commit", "-q", "-am", "fix(storage): the settled fix")
            pinned_sha, pinned_tree = git("rev-parse", "HEAD"), git("rev-parse", "HEAD^{tree}")
            (root / "f").write_text("head\n"); git("commit", "-q", "-am", "test(hil): later work")
            git("checkout", "-q", "-B", "main", git("rev-parse", "pr"))
            git("reset", "-q", "--soft", base_sha)
            git("commit", "-q", "--allow-empty", "-m", "fix(storage): the settled fix")      # the PR title
            squash = git("rev-parse", "HEAD")
            pin = ("0000000", "fix(storage): the settled fix", pinned_tree)
            self.assertEqual(pinned_revs.resolve(pin, root), pinned_sha)
            self.assertNotEqual(pinned_revs.resolve(pin, root), squash)
            git("branch", "-q", "-D", "pr")         # base no longer on any ref: fail closed
            with self.assertRaises(RuntimeError):
                pinned_revs.resolve(pin, root)


if __name__ == "__main__":
    unittest.main()

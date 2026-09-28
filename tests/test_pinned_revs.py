"""pinned_revs: the Sprint 1/2 base commits resolve to content-identical trees
(original or re-signed twin), and a subject match with a different tree is refused."""
from __future__ import annotations

import sys
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


if __name__ == "__main__":
    unittest.main()

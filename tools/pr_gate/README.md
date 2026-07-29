# Firmware PR gate helpers

This directory is the repository-local boundary between a fork-safe GitHub
Actions pull-request event and the pure release analyzer in
`tools/release-analysis`.

`cli.py collect` asserts the exact checked-out head, reads the PR base and tags
from local Git, and uses only read-only GitHub REST calls for immutable release,
merged-PR, and recovery-tag protection facts. It writes the structured input for
`analyze-candidate`. Recovery intent is recognized only when the fixed
`release-recovery` label is present and the validated title has the exact form:

```text
revert(release-<40 lowercase hex SHA>): <non-empty subject>
```

The target still has to be the base's sole unresolved release-bearing merge.
Its first parent is resolved from Git, and the candidate tree must equal an
isolated `git revert <failed-sha>` applied to the exact PR base. The label and PR
body are never proof or candidate identity.

`cli.py prepare` converts a successful analysis into canonical notes and Ticket
2 candidate metadata under `RUNNER_TEMP`. The packager turns that metadata into
a manifest binding the repository, full title/source identity, workflow SHA,
run ID, run attempt, and its deterministic artifact name. GitHub's post-upload
artifact ID is deliberately absent.

Before packaging, `prepare` uses Git name-status facts for the exact base→head
diff (including both sides of renames). A release-bearing candidate that adds,
changes, deletes, or renames a path under `.github/workflows/**` fails with an
instruction to split automation changes into a separate no-release `ci:` PR.
This keeps the trusted publisher on the minimal token: GitHub does not let the
Actions `GITHUB_TOKEN` gain the extra Workflows(write) permission needed to tag
a commit that itself changes workflow files. Ordinary no-release automation
changes, including the bootstrap PR, remain allowed.

The collector shares the analyzer's one-time bootstrap boundary: the exact
published `v1.0.5` tag at
`508bca7c302c8a5e1b5214d5b03d243de6965ac6` starts associated-PR collection
even when GitHub reports that legacy release as mutable. No other mutable tag or
release gains authority from this exception.

Run the local tests with:

```sh
python3 -m unittest discover -s tools/pr_gate/tests -v
```

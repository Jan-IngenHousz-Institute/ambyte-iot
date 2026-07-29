# Firmware main publisher

`firmware-main-publisher.yml` is the only trusted release writer. It never builds
firmware. It resolves the exact merged PR, enumerates successful runs of the exact
PR gate for that PR head, downloads by GitHub artifact ID, and runs the complete
candidate verifier before any tag or release mutation.

The target squash commit must retain GitHub's configured `PR_TITLE` subject form
`<validated title> (#<PR number>)`. That immutable subject freezes the merge-time
title without trusting later PR-title edits. The candidate title remains the
release authority and must match that witness; the selected run's `display_title`
must match it too. The manifest's `workflow.sha` must equal the commit resolved for
`refs/pull/<number>/merge` through GitHub's Git ref API.

Normal no-release push events exit before artifact or PR-merge-ref lookup. This is
intentional for the bootstrap merge titled `ci: add firmware release automation`,
whose PR predates the gate artifact. A manual dispatch explicitly selects a run
and artifact, so even a no-release retry downloads the candidate and verifies the
same frozen title, run title, and workflow provenance without publishing anything.

## Retry and recovery

Prefer **Re-run all jobs** on the original failed push workflow. That preserves
the original `github.sha` automatically.

If a manual dispatch is necessary, all three inputs are mandatory:

- the full 40-character merged `target_sha` on the current `main` first-parent
  chain;
- the positive successful PR-gate workflow run ID;
- the positive GitHub artifact ID belonging to that run.

There is no default to the current branch tip and no artifact lookup by newest
name. Missing, expired, or invalid artifacts are never rebuilt on `main`. Use the
verified revert → protected `release-aborted/<failed-sha>` marker → fresh re-land
flow instead.

Before a draft, asset, release, or recovery-tag mutation, the publisher calls the
immutable-release capability endpoint using the existing minimal Actions token.
Only the exact supported HTTP 200 response with `enabled: true` proceeds. Ticket 6
must enable this setting and exercise the preflight with the live Actions token;
403/404 pauses activation for the explicit GitHub App decision described in the
rollout plan.

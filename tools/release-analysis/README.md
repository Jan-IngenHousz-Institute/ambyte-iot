# Ambyte release analysis

This directory is the repository-local, pure release-analysis boundary shared by
future pull-request and trusted-publisher workflows. It has no GitHub client,
does not run `git`, and performs no writes. Callers collect Git/GitHub facts as
structured JSON; this tool validates those facts and emits structured JSON.

Install and verify the exact locked dependencies:

```sh
npm ci --ignore-scripts
npm test
```

Machine interface:

```sh
node src/cli.mjs check-base < base-state.json
node src/cli.mjs analyze-candidate < candidate.json
node src/cli.mjs check-predecessor < predecessor.json
```

Success writes one compact JSON object to stdout. Failure writes one compact
JSON error object to stdout and exits non-zero. No workflow should parse log or
human-oriented command output.

The first-parent list in each input is ordered oldest to newest and ends at
`base_sha`. Reachability is derived only from membership in that chain. PR title,
number, and canonical pull-request URL are the release authority; commit messages,
PR bodies, labels, dates, and merge actors are deliberately absent from the input
schemas.

Recovery is intentionally narrow. A recovery candidate must be a non-breaking
`revert` title, identify an unresolved release-bearing first-parent commit, name
that commit's first parent, and match the tree produced by the caller's isolated
`git revert` verification. A merged recovery settles the failed commit only when
an exact protected `release-aborted/<failed-sha>` marker points at that verified
recovery merge.

Release state has one exact bootstrap compatibility exception. The published
legacy `v1.0.5` release is accepted only at
`508bca7c302c8a5e1b5214d5b03d243de6965ac6`, even though that pre-existing
release is mutable. Published mutable releases with lower versions at earlier
first-parent positions are historical behind that boundary. Every other
mutable or unpublished release at or after it fails closed; all future release
authority remains published and immutable.

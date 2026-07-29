import assert from "node:assert/strict";
import test from "node:test";
import { analyzeCandidate, canonicalJson } from "../src/index.mjs";
import { candidateFor, fixture } from "./helpers.mjs";

test("candidate output binds deterministic decision, version, notes, and identity", () => {
  const input = candidateFor(fixture("settled-bootstrap"));
  const result = analyzeCandidate(input);

  assert.deepEqual(result.decision, {
    release: true,
    bump: "minor",
    kind: "release",
    reason: "type-feat",
  });
  assert.deepEqual(result.version, {
    previous: "1.0.5",
    next: "1.1.0",
    tag: "v1.1.0",
  });
  assert.equal(result.candidate_identity.release_tag, "v1.1.0");
  assert.equal(result.candidate_identity.title, input.pull_request.title);
  assert.equal(result.candidate_identity.head_sha, input.candidate.head_sha);
  assert.equal(result.notes.markdown.includes(input.pull_request.url), true);
});

test("identical immutable inputs produce byte-identical canonical output", () => {
  const input = candidateFor(fixture("settled-bootstrap"));
  const first = canonicalJson(analyzeCandidate(structuredClone(input)));
  const second = canonicalJson(analyzeCandidate(structuredClone(input)));
  assert.equal(first, second);
});

test("release-bearing candidate is blocked by an unresolved predecessor", () => {
  const input = candidateFor(fixture("unsettled-release"));
  assert.throws(() => analyzeCandidate(input), {
    code: "PREVIOUS_RELEASE_UNSETTLED",
  });
});

test("ordinary no-release candidate may proceed while a release is repaired", () => {
  const input = candidateFor(fixture("unsettled-release"), {
    pull_request: {
      title: "ci: repair release tooling",
    },
  });
  const result = analyzeCandidate(input);
  assert.equal(result.base_state.settled, false);
  assert.deepEqual(result.decision, {
    release: false,
    bump: null,
    kind: "no-release",
    reason: "no-release-type",
  });
  assert.deepEqual(result.version, {
    previous: "1.0.5",
    next: null,
    tag: null,
  });
});

test("no-release canary proceeds from the exact mutable legacy bootstrap boundary", () => {
  const input = candidateFor(fixture("legacy-mutable-bootstrap"), {
    pull_request: {
      title: "ci: exercise firmware release canary",
    },
  });
  const result = analyzeCandidate(input);
  assert.equal(result.base_state.settled, true);
  assert.deepEqual(result.decision, {
    release: false,
    bump: null,
    kind: "no-release",
    reason: "no-release-type",
  });
  assert.equal(result.candidate_identity.latest_release_tag, "v1.0.5");
});

test("verified recovery candidate is the narrow no-release revert exception", () => {
  const input = candidateFor(fixture("unsettled-release"), {
    pull_request: {
      title: "revert: recover failed firmware release",
    },
    candidate: {
      tree_sha: "dddddddddddddddddddddddddddddddddddddddd",
    },
    recovery: {
      failed_sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      failed_parent_sha: "b98ef489614e9c6d98af70020f1adaaf36b1630b",
      expected_revert_tree_sha: "dddddddddddddddddddddddddddddddddddddddd",
    },
  });
  const result = analyzeCandidate(input);
  assert.deepEqual(result.decision, {
    release: false,
    bump: null,
    kind: "recovery",
    reason: "verified-recovery-revert",
  });
  assert.equal(
    result.candidate_identity.recovery_of_sha,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  );
  assert.equal(result.version.tag, null);
});

test("recovery candidate rejects a mismatched exact tree", () => {
  const input = candidateFor(fixture("unsettled-release"), {
    pull_request: {
      title: "revert: recover failed firmware release",
    },
    recovery: {
      failed_sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      failed_parent_sha: "b98ef489614e9c6d98af70020f1adaaf36b1630b",
      expected_revert_tree_sha: "dddddddddddddddddddddddddddddddddddddddd",
    },
  });
  assert.throws(() => analyzeCandidate(input), { code: "INVALID_RECOVERY" });
});

test("recovery candidate rejects non-revert and breaking revert titles", () => {
  for (const title of ["fix: recover release", "revert!: recover release"]) {
    const input = candidateFor(fixture("unsettled-release"), {
      pull_request: { title },
      candidate: {
        tree_sha: "dddddddddddddddddddddddddddddddddddddddd",
      },
      recovery: {
        failed_sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        failed_parent_sha: "b98ef489614e9c6d98af70020f1adaaf36b1630b",
        expected_revert_tree_sha: "dddddddddddddddddddddddddddddddddddddddd",
      },
    });
    assert.throws(() => analyzeCandidate(input), { code: "INVALID_RECOVERY" });
  }
});

test("candidate identity rejects a base mismatch", () => {
  const input = candidateFor(fixture("settled-bootstrap"));
  input.candidate.base_sha = "9999999999999999999999999999999999999999";
  assert.throws(() => analyzeCandidate(input), {
    code: "INVALID_CANDIDATE_IDENTITY",
  });
});

test("candidate schema rejects mutable PR body and unknown identity fields", () => {
  const input = candidateFor(fixture("settled-bootstrap"));
  input.pull_request.body = "mutable release notes";
  input.candidate.run_id = 123;
  assert.throws(() => analyzeCandidate(input), { code: "INVALID_INPUT" });
});

test("malformed associated PR metadata blocks even a no-release candidate", () => {
  const base = fixture("unsettled-release");
  base.first_parent_commits.at(-1).pull_request.title = "not conventional";
  const input = candidateFor(base, {
    pull_request: { title: "ci: repair release tooling" },
  });
  assert.throws(() => analyzeCandidate(input), { code: "INVALID_BASE_STATE" });
});

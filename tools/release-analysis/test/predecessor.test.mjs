import assert from "node:assert/strict";
import test from "node:test";
import {
  analyzeCandidate,
  checkPredecessorState,
} from "../src/index.mjs";
import { candidateFor, fixture } from "./helpers.mjs";

function predecessorInput(candidateInput, analysis) {
  return {
    schema_version: 1,
    candidate_identity: analysis.candidate_identity,
    expected_decision: analysis.decision,
    expected_version: analysis.version,
    merged_commit: {
      sha: "5555555555555555555555555555555555555555",
      parent_sha: candidateInput.candidate.base_sha,
      tree_sha: candidateInput.candidate.tree_sha,
      pull_request: structuredClone(candidateInput.pull_request),
    },
    predecessor: structuredClone(candidateInput.base),
  };
}

test("publisher predecessor check re-derives the exact candidate analysis", () => {
  const candidateInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(candidateInput);
  const result = checkPredecessorState(
    predecessorInput(candidateInput, analysis),
  );

  assert.equal(result.ready, true);
  assert.equal(result.predecessor_sha, candidateInput.candidate.base_sha);
  assert.equal(result.latest_release_tag, "v1.0.5");
  assert.equal(result.release_tag, "v1.1.0");
  assert.deepEqual(result.decision, analysis.decision);
});

test("predecessor comparison is independent of JSON property order", () => {
  const candidateInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(candidateInput);
  const input = predecessorInput(candidateInput, analysis);
  input.expected_decision = {
    reason: analysis.decision.reason,
    kind: analysis.decision.kind,
    bump: analysis.decision.bump,
    release: analysis.decision.release,
  };
  input.expected_version = {
    tag: analysis.version.tag,
    next: analysis.version.next,
    previous: analysis.version.previous,
  };

  assert.equal(checkPredecessorState(input).ready, true);
});

test("predecessor check rejects a merge whose first parent changed", () => {
  const candidateInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(candidateInput);
  const input = predecessorInput(candidateInput, analysis);
  input.merged_commit.parent_sha = "9999999999999999999999999999999999999999";

  assert.throws(() => checkPredecessorState(input), {
    code: "PREDECESSOR_MISMATCH",
  });
});

test("predecessor check rejects latest-tag drift", () => {
  const candidateInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(candidateInput);
  const input = predecessorInput(candidateInput, analysis);
  input.candidate_identity.latest_release_tag = "v1.0.4";

  assert.throws(() => checkPredecessorState(input), {
    code: "PREDECESSOR_MISMATCH",
  });
});

test("predecessor check rejects changed notes identity", () => {
  const candidateInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(candidateInput);
  const input = predecessorInput(candidateInput, analysis);
  input.candidate_identity.notes_sha256 = "9".repeat(64);

  assert.throws(() => checkPredecessorState(input), {
    code: "CANDIDATE_ANALYSIS_MISMATCH",
  });
});

test("predecessor check preserves the verified recovery no-release decision", () => {
  const candidateInput = candidateFor(fixture("unsettled-release"), {
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
  const analysis = analyzeCandidate(candidateInput);
  const result = checkPredecessorState(
    predecessorInput(candidateInput, analysis),
  );

  assert.equal(result.ready, true);
  assert.equal(result.decision.kind, "recovery");
  assert.equal(result.release_tag, null);
});

test("predecessor check blocks a release when the predecessor is unsettled", () => {
  const settledInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(settledInput);
  const unsettled = fixture("unsettled-release");
  const input = predecessorInput(settledInput, analysis);
  input.predecessor = unsettled;
  input.candidate_identity.base_sha = unsettled.base_sha;
  input.merged_commit.parent_sha = unsettled.base_sha;

  assert.throws(() => checkPredecessorState(input), {
    code: "PREVIOUS_RELEASE_UNSETTLED",
  });
});

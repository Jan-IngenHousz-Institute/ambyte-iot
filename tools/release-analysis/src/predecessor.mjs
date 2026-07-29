import { ReleaseAnalysisError } from "./errors.mjs";
import { canonicalJson } from "./json.mjs";
import { canonicalNotes } from "./notes.mjs";
import { assertOutputSchema, validateSchema } from "./schema.mjs";
import { checkBaseState } from "./state.mjs";
import { decisionForTitle, parseTitle } from "./title.mjs";
import { nextVersion } from "./version.mjs";

function sameJson(left, right) {
  return canonicalJson(left) === canonicalJson(right);
}

function recoveryDecision(identity, parsedTitle, predecessorState) {
  if (!identity.recovery_of_sha) {
    return null;
  }
  if (parsedTitle.type !== "revert" || parsedTitle.breaking) {
    throw new ReleaseAnalysisError(
      "PREDECESSOR_MISMATCH",
      "Recovery identity must use a non-breaking revert PR title",
    );
  }
  if (
    predecessorState.errors.length > 0 ||
    predecessorState.unresolved.length !== 1 ||
    predecessorState.unresolved[0].sha !== identity.recovery_of_sha
  ) {
    throw new ReleaseAnalysisError(
      "PREDECESSOR_MISMATCH",
      "Recovery identity does not name the predecessor's only unresolved release",
      predecessorState.unresolved,
    );
  }
  return {
    release: false,
    bump: null,
    kind: "recovery",
    reason: "verified-recovery-revert",
  };
}

export function checkPredecessorState(rawInput) {
  const input = validateSchema("predecessor-check-input.schema.json", rawInput);
  const identity = input.candidate_identity;
  const merged = input.merged_commit;

  const identityMismatches = [];
  const compare = (field, expected, actual) => {
    if (expected !== actual) {
      identityMismatches.push({ field, expected, actual });
    }
  };
  compare("merged_commit.parent_sha", identity.base_sha, merged.parent_sha);
  compare("predecessor.base_sha", identity.base_sha, input.predecessor.base_sha);
  compare("merged_commit.tree_sha", identity.tree_sha, merged.tree_sha);
  compare(
    "merged_commit.pull_request.number",
    identity.pull_request_number,
    merged.pull_request.number,
  );
  compare(
    "merged_commit.pull_request.url",
    identity.pull_request_url,
    merged.pull_request.url,
  );
  compare("merged_commit.pull_request.title", identity.title, merged.pull_request.title);
  if (identityMismatches.length > 0) {
    throw new ReleaseAnalysisError(
      "PREDECESSOR_MISMATCH",
      "Merged commit does not match the candidate's exact predecessor and PR identity",
      identityMismatches,
    );
  }

  const predecessorState = checkBaseState(input.predecessor);
  if (predecessorState.errors.length > 0) {
    throw new ReleaseAnalysisError(
      "INVALID_BASE_STATE",
      "Predecessor state contains validation errors",
      predecessorState.errors,
    );
  }
  compare(
    "latest_release_tag",
    identity.latest_release_tag,
    predecessorState.latest_release.tag,
  );
  if (identityMismatches.length > 0) {
    throw new ReleaseAnalysisError(
      "PREDECESSOR_MISMATCH",
      "Latest immutable release changed after candidate analysis",
      identityMismatches,
    );
  }

  const parsedTitle = parseTitle(merged.pull_request.title);
  const decision =
    recoveryDecision(identity, parsedTitle, predecessorState) ??
    decisionForTitle(parsedTitle);
  if (decision.release && !predecessorState.settled) {
    throw new ReleaseAnalysisError(
      "PREVIOUS_RELEASE_UNSETTLED",
      "A previous release-bearing merge is unresolved",
      predecessorState.unresolved,
    );
  }

  const version = nextVersion(predecessorState.latest_release.tag, decision);
  const notes = canonicalNotes(parsedTitle, merged.pull_request);
  const expectedMismatches = [];
  if (!sameJson(decision, input.expected_decision)) {
    expectedMismatches.push({
      field: "expected_decision",
      expected: input.expected_decision,
      actual: decision,
    });
  }
  if (!sameJson(version, input.expected_version)) {
    expectedMismatches.push({
      field: "expected_version",
      expected: input.expected_version,
      actual: version,
    });
  }
  compare("release_tag", identity.release_tag, version.tag);
  compare("notes_sha256", identity.notes_sha256, notes.sha256);
  expectedMismatches.push(...identityMismatches);
  if (expectedMismatches.length > 0) {
    throw new ReleaseAnalysisError(
      "CANDIDATE_ANALYSIS_MISMATCH",
      "Candidate decision, version, or canonical notes changed",
      expectedMismatches,
    );
  }

  const result = {
    schema_version: 1,
    ok: true,
    ready: true,
    merged_sha: merged.sha,
    predecessor_sha: merged.parent_sha,
    latest_release_tag: predecessorState.latest_release.tag,
    release_tag: version.tag,
    decision,
    version,
    predecessor_state: predecessorState,
  };
  return assertOutputSchema("predecessor-check-output.schema.json", result);
}

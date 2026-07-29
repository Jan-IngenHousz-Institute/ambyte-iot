import { canonicalNotes } from "./notes.mjs";
import { ReleaseAnalysisError } from "./errors.mjs";
import { assertOutputSchema, validateSchema } from "./schema.mjs";
import { checkBaseState } from "./state.mjs";
import { decisionForTitle, parseTitle } from "./title.mjs";
import { nextVersion } from "./version.mjs";

function verifyRecoveryCandidate(input, parsedTitle, baseState) {
  const recovery = input.recovery;
  if (!recovery) {
    return null;
  }
  if (parsedTitle.type !== "revert" || parsedTitle.breaking) {
    throw new ReleaseAnalysisError(
      "INVALID_RECOVERY",
      "Recovery candidate must use a non-breaking revert title",
    );
  }
  if (baseState.errors.length > 0) {
    throw new ReleaseAnalysisError(
      "INVALID_RECOVERY",
      "Recovery candidate cannot proceed while base-state validation has errors",
      baseState.errors,
    );
  }
  if (
    baseState.unresolved.length !== 1 ||
    baseState.unresolved[0].sha !== recovery.failed_sha
  ) {
    throw new ReleaseAnalysisError(
      "INVALID_RECOVERY",
      "Recovery target must be the base's only unresolved release-bearing merge",
      baseState.unresolved,
    );
  }

  const failed = input.base.first_parent_commits.find(
    (commit) => commit.sha === recovery.failed_sha,
  );
  if (!failed || failed.parent_sha !== recovery.failed_parent_sha) {
    throw new ReleaseAnalysisError(
      "INVALID_RECOVERY",
      "Recovery target parent does not match the failed merge's first parent",
    );
  }
  if (input.candidate.tree_sha !== recovery.expected_revert_tree_sha) {
    throw new ReleaseAnalysisError(
      "INVALID_RECOVERY",
      "Candidate tree does not match the verified git-revert tree",
      [
        {
          expected_tree_sha: recovery.expected_revert_tree_sha,
          actual_tree_sha: input.candidate.tree_sha,
        },
      ],
    );
  }

  return {
    release: false,
    bump: null,
    kind: "recovery",
    reason: "verified-recovery-revert",
  };
}

export function analyzeCandidate(rawInput) {
  const input = validateSchema("candidate-analysis-input.schema.json", rawInput);
  if (input.base.base_sha !== input.candidate.base_sha) {
    throw new ReleaseAnalysisError(
      "INVALID_CANDIDATE_IDENTITY",
      "candidate.base_sha must match base.base_sha",
    );
  }

  const baseState = checkBaseState(input.base);
  const parsedTitle = parseTitle(input.pull_request.title);
  const normalDecision = decisionForTitle(parsedTitle);
  const recoveryDecision = verifyRecoveryCandidate(input, parsedTitle, baseState);
  const decision = recoveryDecision ?? normalDecision;

  if (baseState.errors.length > 0) {
    throw new ReleaseAnalysisError(
      "INVALID_BASE_STATE",
      "Candidate base-state analysis contains validation errors",
      baseState.errors,
    );
  }
  if (decision.release && !baseState.settled) {
    throw new ReleaseAnalysisError(
      "PREVIOUS_RELEASE_UNSETTLED",
      "A previous release-bearing merge is unresolved",
      baseState.unresolved,
    );
  }

  const version = nextVersion(baseState.latest_release.tag, decision);
  const notes = canonicalNotes(parsedTitle, input.pull_request);
  const result = {
    schema_version: 1,
    ok: true,
    pull_request: {
      number: input.pull_request.number,
      url: input.pull_request.url,
      title: input.pull_request.title,
      parsed_title: parsedTitle,
    },
    decision,
    version,
    notes,
    candidate_identity: {
      pull_request_number: input.pull_request.number,
      pull_request_url: input.pull_request.url,
      title: input.pull_request.title,
      head_sha: input.candidate.head_sha,
      base_sha: input.candidate.base_sha,
      tree_sha: input.candidate.tree_sha,
      latest_release_tag: baseState.latest_release.tag,
      release_tag: version.tag,
      notes_sha256: notes.sha256,
      recovery_of_sha: input.recovery?.failed_sha ?? null,
    },
    base_state: baseState,
  };

  return assertOutputSchema("candidate-analysis-output.schema.json", result);
}

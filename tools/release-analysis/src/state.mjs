import semver from "semver";
import { APPROVED_BASELINE } from "./constants.mjs";
import { ReleaseAnalysisError } from "./errors.mjs";
import { validatePullRequestUrl } from "./notes.mjs";
import { assertOutputSchema, validateSchema } from "./schema.mjs";
import { decisionForTitle, parseTitle } from "./title.mjs";
import { versionFromTag } from "./version.mjs";

function stateError(code, sha, message) {
  return { code, sha, message };
}

function validateFirstParentChain(input) {
  const commits = input.first_parent_commits;
  const seen = new Set();

  for (let index = 0; index < commits.length; index += 1) {
    const commit = commits[index];
    if (seen.has(commit.sha)) {
      throw new ReleaseAnalysisError(
        "INVALID_HISTORY",
        "First-parent history contains a duplicate commit",
        [{ sha: commit.sha }],
      );
    }
    seen.add(commit.sha);

    if (index > 0 && commit.parent_sha !== commits[index - 1].sha) {
      throw new ReleaseAnalysisError(
        "INVALID_HISTORY",
        "First-parent history is not a contiguous oldest-to-newest chain",
        [
          {
            sha: commit.sha,
            expected_parent_sha: commits[index - 1].sha,
            actual_parent_sha: commit.parent_sha,
          },
        ],
      );
    }
  }

  if (commits.at(-1).sha !== input.base_sha) {
    throw new ReleaseAnalysisError(
      "INVALID_HISTORY",
      "base_sha must be the final commit in first_parent_commits",
      [{ base_sha: input.base_sha, final_sha: commits.at(-1).sha }],
    );
  }
}

function discoverLatestRelease(input, errors) {
  const indexBySha = new Map(
    input.first_parent_commits.map((commit, index) => [commit.sha, index]),
  );
  const seenNames = new Set();
  const reachableRecords = [];

  for (const tag of input.release_tags) {
    if (seenNames.has(tag.name)) {
      throw new ReleaseAnalysisError(
        "INVALID_RELEASE_TAG_STATE",
        "Release tag input contains a duplicate tag name",
        [{ tag: tag.name }],
      );
    }
    seenNames.add(tag.name);

    const index = indexBySha.get(tag.target_sha);
    if (index === undefined) {
      continue;
    }
    reachableRecords.push({
      ...tag,
      index,
      version: versionFromTag(tag.name),
    });
  }

  // This is the sole legacy exception. The exact v1.0.5 release predates
  // immutable releases, but its tag and target were independently approved as
  // the bootstrap boundary. Nothing else inherits this exception.
  const legacyBoundary = reachableRecords.find(
    (record) =>
      record.name === APPROVED_BASELINE.releaseTag &&
      record.target_sha === APPROVED_BASELINE.releaseSha &&
      record.published,
  );
  const reachable = [];

  for (const record of reachableRecords) {
    if (record.published && record.immutable) {
      reachable.push(record);
      continue;
    }
    if (record === legacyBoundary) {
      reachable.push(record);
      continue;
    }

    const isHistoricalLegacyRelease =
      legacyBoundary &&
      record.published &&
      !record.immutable &&
      record.index < legacyBoundary.index &&
      semver.lt(record.version, legacyBoundary.version);
    if (isHistoricalLegacyRelease) {
      continue;
    }

    errors.push(
      stateError(
        "UNTRUSTED_RELEASE_TAG",
        record.target_sha,
        `Reachable release tag ${record.name} is neither the exact approved legacy boundary nor published and immutable`,
      ),
    );
  }

  reachable.sort((left, right) => left.index - right.index);
  if (reachable.length === 0) {
    throw new ReleaseAnalysisError(
      "NO_REACHABLE_RELEASE_TAG",
      "No trusted vX.Y.Z release boundary is reachable from base_sha",
    );
  }

  for (let index = 1; index < reachable.length; index += 1) {
    if (
      reachable[index].index <= reachable[index - 1].index ||
      !semver.gt(reachable[index].version, reachable[index - 1].version)
    ) {
      throw new ReleaseAnalysisError(
        "INVALID_RELEASE_TAG_STATE",
        "Reachable release tags must advance version and first-parent position",
        [
          {
            previous: reachable[index - 1].name,
            next: reachable[index].name,
          },
        ],
      );
    }
  }

  return reachable.at(-1);
}

function isApprovedBaselineCommit(commit, latestRelease) {
  if (
    latestRelease.name !== APPROVED_BASELINE.releaseTag ||
    latestRelease.target_sha !== APPROVED_BASELINE.releaseSha ||
    commit.pull_request ||
    commit.recovery
  ) {
    return false;
  }

  return APPROVED_BASELINE.commits.some(
    (approved) =>
      commit.sha === approved.sha &&
      commit.parent_sha === approved.parentSha &&
      commit.subject === approved.subject,
  );
}

function verifyRecoveryCommit(record, recordsBySha) {
  const commit = record.commit;
  const recovery = commit.recovery;
  if (!recovery) {
    return { valid: false, reason: "Recovery metadata is absent" };
  }

  const failed = recordsBySha.get(recovery.failed_sha);
  if (!failed || failed.index >= record.index) {
    return {
      valid: false,
      reason: "Recovery target is not an earlier first-parent commit after the latest release",
    };
  }
  if (!failed.decision?.release) {
    return {
      valid: false,
      reason: "Recovery target is not a release-bearing PR merge",
    };
  }
  if (record.parsed.type !== "revert" || record.parsed.breaking) {
    return {
      valid: false,
      reason: "Recovery PR must use a non-breaking revert title",
    };
  }
  if (recovery.failed_parent_sha !== failed.commit.parent_sha) {
    return {
      valid: false,
      reason: "Recovery target parent does not match the failed merge's first parent",
    };
  }
  if (recovery.expected_revert_tree_sha !== commit.tree_sha) {
    return {
      valid: false,
      reason: "Recovery merge tree does not match the verified git-revert tree",
    };
  }

  return { valid: true, failed };
}

export function checkBaseState(rawInput) {
  const input = validateSchema("base-state-input.schema.json", rawInput);
  validateFirstParentChain(input);

  const errors = [];
  const latestRelease = discoverLatestRelease(input, errors);
  const afterLatest = input.first_parent_commits.slice(latestRelease.index + 1);
  const records = [];
  const recordsBySha = new Map();

  for (let offset = 0; offset < afterLatest.length; offset += 1) {
    const commit = afterLatest[offset];
    const index = latestRelease.index + 1 + offset;

    if (isApprovedBaselineCommit(commit, latestRelease)) {
      const record = {
        commit,
        index,
        parsed: null,
        decision: null,
        state: "accepted-baseline",
        recoveryOfSha: null,
      };
      records.push(record);
      recordsBySha.set(commit.sha, record);
      continue;
    }

    if (!commit.pull_request) {
      const record = {
        commit,
        index,
        parsed: null,
        decision: null,
        state: "unresolved-release",
        recoveryOfSha: null,
      };
      errors.push(
        stateError(
          "MISSING_ASSOCIATED_PR",
          commit.sha,
          "First-parent commit after the latest release has no associated merged PR",
        ),
      );
      records.push(record);
      recordsBySha.set(commit.sha, record);
      continue;
    }

    try {
      validatePullRequestUrl(commit.pull_request.url, commit.pull_request.number);
      const parsed = parseTitle(commit.pull_request.title);
      const decision = decisionForTitle(parsed);
      const record = {
        commit,
        index,
        parsed,
        decision,
        state: decision.release ? "unresolved-release" : "settled-no-release",
        recoveryOfSha: null,
      };
      records.push(record);
      recordsBySha.set(commit.sha, record);
    } catch (error) {
      const record = {
        commit,
        index,
        parsed: null,
        decision: null,
        state: "unresolved-release",
        recoveryOfSha: null,
      };
      errors.push(
        stateError(
          error.code ?? "INVALID_ASSOCIATED_PR",
          commit.sha,
          error.message,
        ),
      );
      records.push(record);
      recordsBySha.set(commit.sha, record);
    }
  }

  const verifiedRecoveryByMergeSha = new Map();
  const verifiedRecoveryByFailedSha = new Map();
  for (const record of records.filter((candidate) => candidate.commit.recovery)) {
    if (!record.parsed || !record.decision) {
      errors.push(
        stateError(
          "INVALID_RECOVERY",
          record.commit.sha,
          "Recovery PR metadata is not valid Conventional Commit PR metadata",
        ),
      );
      continue;
    }

    const verification = verifyRecoveryCommit(record, recordsBySha);
    if (!verification.valid) {
      errors.push(
        stateError("INVALID_RECOVERY", record.commit.sha, verification.reason),
      );
      continue;
    }
    if (verifiedRecoveryByFailedSha.has(verification.failed.commit.sha)) {
      errors.push(
        stateError(
          "DUPLICATE_RECOVERY",
          record.commit.sha,
          "More than one verified recovery merge targets the same failed release",
        ),
      );
      continue;
    }

    record.decision = {
      release: false,
      bump: null,
      kind: "recovery",
      reason: "verified-recovery-revert",
    };
    record.state = "verified-recovery";
    record.recoveryOfSha = verification.failed.commit.sha;
    verifiedRecoveryByMergeSha.set(record.commit.sha, record);
    verifiedRecoveryByFailedSha.set(verification.failed.commit.sha, record);
  }

  const invalidRecoveryMarkers = [];
  const seenMarkerNames = new Set();
  for (const marker of input.recovery_markers) {
    let reason = null;
    const failedSha = marker.name.slice("release-aborted/".length);
    const recovery = verifiedRecoveryByMergeSha.get(marker.target_sha);

    if (seenMarkerNames.has(marker.name)) {
      reason = "Duplicate recovery marker name";
    } else if (!marker.protected) {
      reason = "Recovery marker is not protected";
    } else if (!recovery) {
      reason = "Marker target is not a verified recovery merge";
    } else if (recovery.recoveryOfSha !== failedSha) {
      reason = "Marker name does not identify the recovery merge's exact failed SHA";
    }
    seenMarkerNames.add(marker.name);

    if (reason) {
      invalidRecoveryMarkers.push({
        name: marker.name,
        target_sha: marker.target_sha,
        reason,
      });
      continue;
    }

    const failed = recordsBySha.get(failedSha);
    failed.state = "aborted-release";
  }

  const unresolved = records
    .filter((record) => record.state === "unresolved-release" && record.decision?.release)
    .map((record) => ({
      sha: record.commit.sha,
      pull_request_number: record.commit.pull_request.number,
      title: record.commit.pull_request.title,
      bump: record.decision.bump,
    }));

  const result = {
    schema_version: 1,
    ok: true,
    settled: unresolved.length === 0 && errors.length === 0,
    base_sha: input.base_sha,
    latest_release: {
      tag: latestRelease.name,
      version: latestRelease.version,
      target_sha: latestRelease.target_sha,
    },
    commits: records.map((record) => ({
      sha: record.commit.sha,
      pull_request_number: record.commit.pull_request?.number ?? null,
      title: record.commit.pull_request?.title ?? record.commit.subject,
      state: record.state,
      decision: record.decision,
      recovery_of_sha: record.recoveryOfSha,
    })),
    unresolved,
    errors,
    invalid_recovery_markers: invalidRecoveryMarkers,
  };

  return assertOutputSchema("base-state-output.schema.json", result);
}

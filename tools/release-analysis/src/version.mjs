import semver from "semver";
import { ReleaseAnalysisError } from "./errors.mjs";

const EXACT_TAG = /^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$/;

export function versionFromTag(tag) {
  const match = EXACT_TAG.exec(tag);
  if (!match || !semver.valid(match[0].slice(1))) {
    throw new ReleaseAnalysisError(
      "INVALID_RELEASE_TAG",
      "Release tag must be an exact vX.Y.Z semantic version",
      [{ tag }],
    );
  }
  return match[0].slice(1);
}

export function nextVersion(latestTag, decision) {
  const previous = versionFromTag(latestTag);
  if (!decision.release) {
    return { previous, next: null, tag: null };
  }

  const next = semver.inc(previous, decision.bump);
  if (!next) {
    throw new ReleaseAnalysisError(
      "INVALID_VERSION_BUMP",
      "Could not derive the next semantic version",
    );
  }
  return { previous, next, tag: `v${next}` };
}

import { NO_RELEASE_TYPES, RELEASE_TYPES } from "./constants.mjs";
import { ReleaseAnalysisError } from "./errors.mjs";

const ALLOWED_TYPES = [...Object.keys(RELEASE_TYPES), ...NO_RELEASE_TYPES].join("|");
const TITLE_PATTERN = new RegExp(
  `^(?<type>${ALLOWED_TYPES})(?:\\((?<scope>[^()\\s]+)\\))?(?<breaking>!)?: (?<subject>\\S(?:.*\\S)?)$`,
);

export function parseTitle(title) {
  if (typeof title !== "string" || /[\u0000-\u001f\u007f]/u.test(title)) {
    throw new ReleaseAnalysisError(
      "INVALID_TITLE",
      "PR title must be one line of Conventional Commit text without control characters",
    );
  }

  const match = TITLE_PATTERN.exec(title);
  if (!match) {
    throw new ReleaseAnalysisError(
      "INVALID_TITLE",
      "PR title must match '<type>(<scope>)!: <subject>' using an approved type",
      [{ field: "pull_request.title", value: title }],
    );
  }

  return {
    type: match.groups.type,
    scope: match.groups.scope ?? null,
    subject: match.groups.subject,
    breaking: match.groups.breaking === "!",
  };
}

export function decisionForTitle(parsedTitle) {
  if (parsedTitle.breaking) {
    return {
      release: true,
      bump: "major",
      kind: "release",
      reason: "breaking-change",
    };
  }

  const bump = RELEASE_TYPES[parsedTitle.type];
  if (bump) {
    return {
      release: true,
      bump,
      kind: "release",
      reason: `type-${parsedTitle.type}`,
    };
  }

  return {
    release: false,
    bump: null,
    kind: "no-release",
    reason: "no-release-type",
  };
}

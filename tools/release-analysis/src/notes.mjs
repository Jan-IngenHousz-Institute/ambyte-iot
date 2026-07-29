import { createHash } from "node:crypto";
import { TYPE_SECTIONS } from "./constants.mjs";
import { ReleaseAnalysisError } from "./errors.mjs";

function escapeMarkdown(value) {
  return value.replace(/[\\`*_[\]<>]/g, "\\$&");
}

export function validatePullRequestUrl(url, number) {
  let parsed;
  try {
    parsed = new URL(url);
  } catch {
    throw new ReleaseAnalysisError(
      "INVALID_PULL_REQUEST_URL",
      "Pull request URL must be a canonical GitHub pull-request URL",
      [{ url }],
    );
  }

  const pathParts = parsed.pathname.split("/").filter(Boolean);
  const valid =
    parsed.protocol === "https:" &&
    parsed.hostname === "github.com" &&
    parsed.username === "" &&
    parsed.password === "" &&
    parsed.port === "" &&
    parsed.search === "" &&
    parsed.hash === "" &&
    pathParts.length === 4 &&
    pathParts[2] === "pull" &&
    pathParts[3] === String(number);

  if (!valid) {
    throw new ReleaseAnalysisError(
      "INVALID_PULL_REQUEST_URL",
      "Pull request URL must be canonical https://github.com/<owner>/<repo>/pull/<number>",
      [{ url, number }],
    );
  }
}

export function canonicalNotes(parsedTitle, pullRequest) {
  validatePullRequestUrl(pullRequest.url, pullRequest.number);

  const type = escapeMarkdown(parsedTitle.type);
  const scope = parsedTitle.scope
    ? `(${escapeMarkdown(parsedTitle.scope)})`
    : "";
  const breaking = parsedTitle.breaking ? "!" : "";
  const subject = escapeMarkdown(parsedTitle.subject);
  const section = TYPE_SECTIONS[parsedTitle.type];
  const markdown =
    `## ${section}\n\n` +
    `- **${type}${scope}${breaking}:** ${subject} ` +
    `([#${pullRequest.number}](${pullRequest.url}))\n`;

  return {
    format: "text/markdown",
    markdown,
    sha256: createHash("sha256").update(markdown, "utf8").digest("hex"),
  };
}

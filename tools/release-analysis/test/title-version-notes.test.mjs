import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";
import {
  canonicalNotes,
  decisionForTitle,
  nextVersion,
  parseTitle,
  versionFromTag,
} from "../src/index.mjs";

const expectedBumps = {
  feat: "minor",
  fix: "patch",
  perf: "patch",
  revert: "patch",
};

for (const [type, bump] of Object.entries(expectedBumps)) {
  test(`${type} produces a ${bump} release`, () => {
    const parsed = parseTitle(`${type}(sensor): improve sampling`);
    assert.deepEqual(decisionForTitle(parsed), {
      release: true,
      bump,
      kind: "release",
      reason: `type-${type}`,
    });
  });
}

for (const type of [
  "docs",
  "style",
  "chore",
  "refactor",
  "test",
  "build",
  "ci",
]) {
  test(`${type} produces no release`, () => {
    assert.deepEqual(decisionForTitle(parseTitle(`${type}: update tooling`)), {
      release: false,
      bump: null,
      kind: "no-release",
      reason: "no-release-type",
    });
  });

  test(`${type}! overrides the no-release mapping with major`, () => {
    assert.deepEqual(decisionForTitle(parseTitle(`${type}!: break compatibility`)), {
      release: true,
      bump: "major",
      kind: "release",
      reason: "breaking-change",
    });
  });
}

test("breaking scope syntax produces major", () => {
  assert.deepEqual(parseTitle("feat(api)!: replace sensor schema"), {
    type: "feat",
    scope: "api",
    subject: "replace sensor schema",
    breaking: true,
  });
  assert.equal(
    decisionForTitle(parseTitle("feat(api)!: replace sensor schema")).bump,
    "major",
  );
});

for (const title of [
  "feature: add sensor",
  "Feat: add sensor",
  "feat:add sensor",
  "feat(): add sensor",
  "feat(scope): ",
  "feat: trailing space ",
  "feat: embedded\tcontrol",
  "feat: first line\nsecond line",
]) {
  test(`malformed title is rejected: ${JSON.stringify(title)}`, () => {
    assert.throws(() => parseTitle(title), { code: "INVALID_TITLE" });
  });
}

test("versions are exact and deterministic", () => {
  assert.equal(versionFromTag("v1.0.5"), "1.0.5");
  assert.deepEqual(
    nextVersion("v1.0.5", {
      release: true,
      bump: "minor",
      kind: "release",
      reason: "type-feat",
    }),
    { previous: "1.0.5", next: "1.1.0", tag: "v1.1.0" },
  );
  assert.deepEqual(
    nextVersion("v1.0.5", {
      release: false,
      bump: null,
      kind: "no-release",
      reason: "no-release-type",
    }),
    { previous: "1.0.5", next: null, tag: null },
  );
});

for (const tag of ["1.0.5", "v01.0.5", "v1.0", "v1.0.5-rc.1", "v1.0.5+meta"]) {
  test(`non-exact release tag is rejected: ${tag}`, () => {
    assert.throws(() => versionFromTag(tag), { code: "INVALID_RELEASE_TAG" });
  });
}

test("canonical notes use only escaped immutable PR metadata", () => {
  const parsed = parseTitle("feat(sensor): add *stable* [readings]");
  const pullRequest = {
    number: 50,
    title: "feat(sensor): add *stable* [readings]",
    url: "https://github.com/Jan-IngenHousz-Institute/ambyte-iot/pull/50",
  };
  const notes = canonicalNotes(parsed, pullRequest);
  const expected =
    "## Features\n\n" +
    "- **feat(sensor):** add \\*stable\\* \\[readings\\] " +
    "([#50](https://github.com/Jan-IngenHousz-Institute/ambyte-iot/pull/50))\n";

  assert.equal(notes.markdown, expected);
  assert.equal(
    notes.sha256,
    createHash("sha256").update(expected, "utf8").digest("hex"),
  );
  assert.equal(notes.markdown.includes("commit"), false);
});

for (const url of [
  "http://github.com/owner/repo/pull/50",
  "https://github.com/owner/repo/pull/49",
  "https://github.com/owner/repo/pull/50?editable=true",
  "https://example.com/owner/repo/pull/50",
]) {
  test(`non-canonical PR URL is rejected: ${url}`, () => {
    assert.throws(
      () =>
        canonicalNotes(parseTitle("fix: repair sensor"), {
          number: 50,
          title: "fix: repair sensor",
          url,
        }),
      { code: "INVALID_PULL_REQUEST_URL" },
    );
  });
}

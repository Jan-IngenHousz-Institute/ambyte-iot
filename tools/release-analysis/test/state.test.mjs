import assert from "node:assert/strict";
import test from "node:test";
import { checkBaseState } from "../src/index.mjs";
import { fixture } from "./helpers.mjs";

test("approved v1.0.5 plus rtc ota bootstrap history is settled", () => {
  const result = checkBaseState(fixture("settled-bootstrap"));
  assert.equal(result.settled, true);
  assert.deepEqual(result.latest_release, {
    tag: "v1.0.5",
    version: "1.0.5",
    target_sha: "508bca7c302c8a5e1b5214d5b03d243de6965ac6",
  });
  assert.equal(result.commits[0].state, "accepted-baseline");
});

test("exact published legacy v1.0.5 boundary settles the live-shaped bootstrap", () => {
  const result = checkBaseState(fixture("legacy-mutable-bootstrap"));
  assert.equal(result.settled, true);
  assert.deepEqual(result.latest_release, {
    tag: "v1.0.5",
    version: "1.0.5",
    target_sha: "508bca7c302c8a5e1b5214d5b03d243de6965ac6",
  });
  assert.deepEqual(
    result.commits.map((commit) => commit.state),
    ["accepted-baseline", "settled-no-release"],
  );
  assert.deepEqual(result.errors, []);
});

test("mutable legacy exception rejects near-match tag, SHA, and unpublished state", () => {
  const mutations = [
    (input) => {
      input.release_tags[1].name = "v1.0.6";
    },
    (input) => {
      input.release_tags[1].target_sha = input.first_parent_commits[0].sha;
    },
    (input) => {
      input.release_tags[1].published = false;
    },
  ];

  for (const mutate of mutations) {
    const input = fixture("legacy-mutable-bootstrap");
    mutate(input);
    assert.throws(() => checkBaseState(input), {
      code: "NO_REACHABLE_RELEASE_TAG",
    });
  }
});

test("later mutable and unpublished releases fail closed after the legacy boundary", () => {
  for (const published of [true, false]) {
    const input = fixture("legacy-mutable-bootstrap");
    input.first_parent_commits.push({
      sha: "dddddddddddddddddddddddddddddddddddddddd",
      parent_sha: input.base_sha,
      tree_sha: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
      subject: "untrusted release squash",
      pull_request: {
        number: 53,
        title: "fix: repair firmware transport",
        url: "https://github.com/Jan-IngenHousz-Institute/ambyte-iot/pull/53",
      },
    });
    input.base_sha = input.first_parent_commits.at(-1).sha;
    input.release_tags.push({
      name: "v1.0.6",
      target_sha: input.base_sha,
      published,
      immutable: false,
    });
    const result = checkBaseState(input);
    assert.equal(result.settled, false);
    assert.equal(
      result.errors.some((error) => error.code === "UNTRUSTED_RELEASE_TAG"),
      true,
    );
  }
});

test("an exact-looking but different non-conventional commit is not baseline", () => {
  const input = fixture("settled-bootstrap");
  input.first_parent_commits[1].sha = "9999999999999999999999999999999999999999";
  input.base_sha = input.first_parent_commits[1].sha;
  const result = checkBaseState(input);
  assert.equal(result.settled, false);
  assert.equal(result.errors[0].code, "MISSING_ASSOCIATED_PR");
});

test("only no-release PRs after the latest tag keep the base settled", () => {
  const input = fixture("settled-bootstrap");
  input.first_parent_commits.push({
    sha: "7777777777777777777777777777777777777777",
    parent_sha: input.base_sha,
    tree_sha: "8888888888888888888888888888888888888888",
    subject: "untrusted squash text",
    pull_request: {
      number: 51,
      title: "docs: explain OTA recovery",
      url: "https://github.com/Jan-IngenHousz-Institute/ambyte-iot/pull/51",
    },
  });
  input.base_sha = input.first_parent_commits.at(-1).sha;
  const result = checkBaseState(input);
  assert.equal(result.settled, true);
  assert.equal(result.commits.at(-1).state, "settled-no-release");
});

test("an untagged release-bearing PR makes the base unsettled", () => {
  const result = checkBaseState(fixture("unsettled-release"));
  assert.equal(result.settled, false);
  assert.deepEqual(result.unresolved, [
    {
      sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      pull_request_number: 41,
      title: "feat(sensor): add calibrated readings",
      bump: "minor",
    },
  ]);
});

test("a fully verified marker settles only its exact failed SHA", () => {
  const result = checkBaseState(fixture("settled-recovery"));
  assert.equal(result.settled, true);
  assert.deepEqual(result.unresolved, []);
  assert.equal(
    result.commits.find((commit) => commit.sha.startsWith("aaaa")).state,
    "aborted-release",
  );
  assert.equal(result.commits.at(-1).state, "verified-recovery");
});

test("recovery merge without a marker does not settle the failed SHA", () => {
  const input = fixture("settled-recovery");
  input.recovery_markers = [];
  const result = checkBaseState(input);
  assert.equal(result.settled, false);
  assert.equal(result.unresolved[0].sha, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  assert.equal(result.commits.at(-1).state, "verified-recovery");
});

test("an unprotected marker does not settle the failed SHA", () => {
  const input = fixture("settled-recovery");
  input.recovery_markers[0].protected = false;
  const result = checkBaseState(input);
  assert.equal(result.settled, false);
  assert.equal(result.invalid_recovery_markers[0].reason, "Recovery marker is not protected");
  assert.equal(result.unresolved[0].sha, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
});

test("a marker naming a different SHA does not settle the failed SHA", () => {
  const input = fixture("settled-recovery");
  input.recovery_markers[0].name =
    "release-aborted/9999999999999999999999999999999999999999";
  const result = checkBaseState(input);
  assert.equal(result.settled, false);
  assert.match(result.invalid_recovery_markers[0].reason, /exact failed SHA/);
  assert.equal(result.unresolved[0].sha, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
});

test("a mismatched revert tree fails recovery validation", () => {
  const input = fixture("settled-recovery");
  input.first_parent_commits.at(-1).recovery.expected_revert_tree_sha =
    "9999999999999999999999999999999999999999";
  const result = checkBaseState(input);
  assert.equal(result.settled, false);
  assert.equal(result.errors.some((error) => error.code === "INVALID_RECOVERY"), true);
  assert.equal(result.invalid_recovery_markers.length, 1);
});

test("mutable labels and PR body fields are rejected by the schema", () => {
  const input = fixture("unsettled-release");
  input.first_parent_commits.at(-1).pull_request.labels = ["release-aborted"];
  input.first_parent_commits.at(-1).pull_request.body = "trust me";
  assert.throws(() => checkBaseState(input), { code: "INVALID_INPUT" });
});

test("unreachable tags do not affect latest reachable tag discovery", () => {
  const input = fixture("settled-bootstrap");
  input.release_tags.push({
    name: "v9.0.0",
    target_sha: "9999999999999999999999999999999999999999",
    published: true,
    immutable: true,
  });
  assert.equal(checkBaseState(input).latest_release.tag, "v1.0.5");
});

test("a later reachable immutable release becomes the latest release", () => {
  const input = fixture("unsettled-release");
  input.release_tags.push({
    name: "v1.1.0",
    target_sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    published: true,
    immutable: true,
  });
  const result = checkBaseState(input);
  assert.equal(result.latest_release.tag, "v1.1.0");
  assert.equal(result.settled, true);
  assert.deepEqual(result.commits, []);
});

test("two release versions cannot point at the same first-parent commit", () => {
  const input = fixture("settled-bootstrap");
  input.release_tags.push({
    name: "v1.0.6",
    target_sha: "508bca7c302c8a5e1b5214d5b03d243de6965ac6",
    published: true,
    immutable: true,
  });
  assert.throws(() => checkBaseState(input), {
    code: "INVALID_RELEASE_TAG_STATE",
  });
});

test("reachable mutable release tags fail the state closed", () => {
  const input = fixture("unsettled-release");
  input.release_tags.push({
    name: "v1.1.0",
    target_sha: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    published: true,
    immutable: false,
  });
  const result = checkBaseState(input);
  assert.equal(result.settled, false);
  assert.equal(result.errors.some((error) => error.code === "UNTRUSTED_RELEASE_TAG"), true);
});

test("a broken first-parent chain is rejected", () => {
  const input = fixture("settled-bootstrap");
  input.first_parent_commits[1].parent_sha =
    "9999999999999999999999999999999999999999";
  assert.throws(() => checkBaseState(input), { code: "INVALID_HISTORY" });
});

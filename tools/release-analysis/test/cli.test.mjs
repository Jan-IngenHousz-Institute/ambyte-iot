import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { candidateFor, fixture } from "./helpers.mjs";

const cli = fileURLToPath(new URL("../src/cli.mjs", import.meta.url));

function run(operation, input) {
  return spawnSync(process.execPath, [cli, operation], {
    input: typeof input === "string" ? input : JSON.stringify(input),
    encoding: "utf8",
  });
}

test("CLI emits one structured success object", () => {
  const result = run(
    "analyze-candidate",
    candidateFor(fixture("settled-bootstrap")),
  );
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stderr, "");
  assert.equal(result.stdout.trim().split("\n").length, 1);
  const output = JSON.parse(result.stdout);
  assert.equal(output.ok, true);
  assert.equal(output.version.tag, "v1.1.0");
});

test("CLI exposes the pure predecessor check", async () => {
  const { analyzeCandidate } = await import("../src/index.mjs");
  const candidateInput = candidateFor(fixture("settled-bootstrap"));
  const analysis = analyzeCandidate(candidateInput);
  const input = {
    schema_version: 1,
    candidate_identity: analysis.candidate_identity,
    expected_decision: analysis.decision,
    expected_version: analysis.version,
    merged_commit: {
      sha: "5555555555555555555555555555555555555555",
      parent_sha: candidateInput.candidate.base_sha,
      tree_sha: candidateInput.candidate.tree_sha,
      pull_request: candidateInput.pull_request,
    },
    predecessor: candidateInput.base,
  };
  const result = run("check-predecessor", input);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(result.stdout).ready, true);
});

test("CLI reports malformed JSON without a stack or human log parsing", () => {
  const result = run("check-base", "{not-json");
  assert.equal(result.status, 1);
  assert.equal(result.stderr, "");
  assert.deepEqual(JSON.parse(result.stdout), {
    error: {
      code: "INVALID_JSON",
      details: [],
      message: "Input is not valid JSON",
    },
    ok: false,
    schema_version: 1,
  });
});

test("CLI reports schema failures as machine-readable JSON", () => {
  const input = fixture("settled-bootstrap");
  input.mutable_label = "trust-this";
  const result = run("check-base", input);
  assert.equal(result.status, 1);
  const output = JSON.parse(result.stdout);
  assert.equal(output.ok, false);
  assert.equal(output.error.code, "INVALID_INPUT");
  assert.equal(Array.isArray(output.error.details), true);
});

test("CLI rejects unknown operations structurally", () => {
  const result = run("publish", {});
  assert.equal(result.status, 1);
  assert.equal(JSON.parse(result.stdout).error.code, "INVALID_OPERATION");
});

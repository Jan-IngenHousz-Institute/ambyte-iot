#!/usr/bin/env node
import { stdin, stdout } from "node:process";
import { analyzeCandidate } from "./candidate.mjs";
import { ReleaseAnalysisError, structuredError } from "./errors.mjs";
import { canonicalJson } from "./json.mjs";
import { checkPredecessorState } from "./predecessor.mjs";
import { assertOutputSchema } from "./schema.mjs";
import { checkBaseState } from "./state.mjs";

const MAX_INPUT_BYTES = 2 * 1024 * 1024;

async function readInput() {
  const chunks = [];
  let bytes = 0;
  for await (const chunk of stdin) {
    bytes += chunk.length;
    if (bytes > MAX_INPUT_BYTES) {
      throw new ReleaseAnalysisError(
        "INPUT_TOO_LARGE",
        `Input exceeds ${MAX_INPUT_BYTES} bytes`,
      );
    }
    chunks.push(chunk);
  }

  try {
    return JSON.parse(Buffer.concat(chunks).toString("utf8"));
  } catch {
    throw new ReleaseAnalysisError("INVALID_JSON", "Input is not valid JSON");
  }
}

async function main() {
  const operation = process.argv[2];
  const input = await readInput();
  if (operation === "check-base") {
    return checkBaseState(input);
  }
  if (operation === "analyze-candidate") {
    return analyzeCandidate(input);
  }
  if (operation === "check-predecessor") {
    return checkPredecessorState(input);
  }
  throw new ReleaseAnalysisError(
    "INVALID_OPERATION",
    "Operation must be 'check-base', 'analyze-candidate', or 'check-predecessor'",
  );
}

try {
  stdout.write(`${canonicalJson(await main())}\n`);
} catch (error) {
  const output = structuredError(error);
  assertOutputSchema("error-output.schema.json", output);
  stdout.write(`${canonicalJson(output)}\n`);
  process.exitCode = 1;
}

#!/usr/bin/env node
import { stdout } from "node:process";
import { canonicalJson } from "../release-analysis/src/json.mjs";
import { decisionForTitle, parseTitle } from "../release-analysis/src/title.mjs";

const title = process.env.AMBYTE_PR_TITLE;
if (typeof title !== "string") {
  throw new Error("AMBYTE_PR_TITLE is required");
}

try {
  const parsed = parseTitle(title);
  stdout.write(
    `${canonicalJson({ ok: true, parsed_title: parsed, decision: decisionForTitle(parsed) })}\n`,
  );
} catch (error) {
  stdout.write(
    `${canonicalJson({
      ok: false,
      code: error?.code ?? "INVALID_TITLE",
      message: error?.message ?? "PR title validation failed",
    })}\n`,
  );
  process.exitCode = 1;
}

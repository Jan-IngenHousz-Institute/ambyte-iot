#!/usr/bin/env node
import { stdin, stdout } from "node:process";
import {
  canonicalJson,
  canonicalNotes,
  decisionForTitle,
  parseTitle,
} from "../release-analysis/src/index.mjs";

const chunks = [];
let bytes = 0;
for await (const chunk of stdin) {
  bytes += chunk.length;
  if (bytes > 64 * 1024) {
    throw new Error("pull-request analysis input exceeds 64 KiB");
  }
  chunks.push(chunk);
}

const input = JSON.parse(Buffer.concat(chunks).toString("utf8"));
const parsedTitle = parseTitle(input.title);
const result = {
  schema_version: 1,
  parsed_title: parsedTitle,
  decision: decisionForTitle(parsedTitle),
  notes: canonicalNotes(parsedTitle, input),
};
stdout.write(`${canonicalJson(result)}\n`);

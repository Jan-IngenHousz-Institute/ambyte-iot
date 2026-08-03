#!/usr/bin/env node

import { pathToFileURL } from "node:url";

export const firmwareBuildRequired = (files) =>
  files.some((file) => file !== "lua" && !file.startsWith("lua/"));

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  let input = "";
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", (chunk) => {
    input += chunk;
  });
  process.stdin.on("end", () => {
    const files = input.split(/\r?\n/).filter(Boolean);
    process.stdout.write(
      `firmware-build-required=${firmwareBuildRequired(files)}\n`,
    );
  });
}

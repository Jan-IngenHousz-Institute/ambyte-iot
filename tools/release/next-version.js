import fs from "node:fs";
import { pathToFileURL } from "node:url";

const VERSION_PATTERN = /\bnext release version is\s+(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)/gi;

export const parseNextReleaseVersion = (log) => {
  const matches = [...log.matchAll(VERSION_PATTERN)];
  return matches.at(-1)?.[1] ?? "";
};

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [logPath] = process.argv.slice(2);
  if (!logPath) {
    console.error("usage: node tools/release/next-version.js <semantic-release-log>");
    process.exitCode = 2;
  } else {
    process.stdout.write(`${parseNextReleaseVersion(fs.readFileSync(logPath, "utf8"))}\n`);
  }
}

import { readFileSync } from "node:fs";

export function fixture(name) {
  return JSON.parse(
    readFileSync(new URL(`./fixtures/${name}.json`, import.meta.url), "utf8"),
  );
}

export function candidateFor(base, overrides = {}) {
  return {
    schema_version: 1,
    pull_request: {
      number: 50,
      title: "feat(sensor): add stable sampling",
      url: "https://github.com/Jan-IngenHousz-Institute/ambyte-iot/pull/50",
      ...overrides.pull_request,
    },
    candidate: {
      head_sha: "3333333333333333333333333333333333333333",
      base_sha: base.base_sha,
      tree_sha: "4444444444444444444444444444444444444444",
      ...overrides.candidate,
    },
    base,
    ...(overrides.recovery ? { recovery: overrides.recovery } : {}),
  };
}

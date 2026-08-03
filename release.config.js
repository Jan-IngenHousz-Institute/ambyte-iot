import {
  analyzeCommits,
  fail,
  generateNotes,
  success,
} from "./tools/release/path-scoped.js";

export const releaseRules = [
  { type: "feat", release: "minor" },
  { type: "fix", release: "patch" },
  { type: "perf", release: "patch" },
  { type: "revert", release: "patch" },
  { type: "docs", release: false },
  { type: "style", release: false },
  { type: "chore", release: false },
  { type: "refactor", release: false },
  { type: "test", release: false },
  { type: "build", release: false },
  { type: "ci", release: false },
  { breaking: true, release: "major" },
];

export default {
  branches: ["main"],
  tagFormat: "v${version}",
  analyzeCommits,
  generateNotes,
  success,
  fail,
  plugins: [
    ["@semantic-release/commit-analyzer", { preset: "conventionalcommits", releaseRules }],
    ["@semantic-release/release-notes-generator", { preset: "conventionalcommits" }],
    [
      "@semantic-release/github",
      {
        assets: [
          { path: "release-assets/firmware.bin", label: "Firmware binary" },
          { path: "release-assets/ambyte-iot-v*.zip", label: "Firmware flash bundle" },
        ],
        successComment: false,
        failComment: false,
        releasedLabels: false,
      },
    ],
  ],
};

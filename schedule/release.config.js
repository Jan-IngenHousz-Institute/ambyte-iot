import firmwareConfig, { releaseRules } from "../release.config.js";
import {
  analyzeCommits,
  fail,
  generateNotes,
  success,
} from "../tools/release/path-scoped.js";

export default {
  ...firmwareConfig,
  tagFormat: "schedule-v${version}",
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
          { path: "../release-assets/default.yaml", label: "Default Ambyte field schedule" },
          { path: "../release-assets/default.yaml.manifest.json", label: "Default schedule manifest" },
          { path: "../release-assets/legacy_1hz_spec.yaml", label: "Legacy 1 Hz field schedule" },
          { path: "../release-assets/legacy_1hz_spec.yaml.manifest.json", label: "Legacy 1 Hz schedule manifest" },
          { path: "../release-assets/actions.schema.json", label: "Schedule action schema" },
        ],
        successComment: false,
        failComment: false,
        releasedLabels: false,
      },
    ],
  ],
};

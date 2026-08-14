import firmwareConfig, { releaseRules } from "../release.config.js";
import {
  analyzeCommits,
  fail,
  generateNotes,
  success,
} from "../tools/release/path-scoped.js";

export default {
  ...firmwareConfig,
  tagFormat: "lua-v${version}",
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
          { path: "../release-assets/main.lua", label: "Default Ambyte field script" },
          { path: "../release-assets/main.lua.manifest.json", label: "Default Lua manifest" },
          { path: "../release-assets/legacy_1Hz_spec.lua", label: "Legacy cmd 31 1 Hz field script" },
          { path: "../release-assets/legacy_1Hz_spec.lua.manifest.json", label: "Legacy cmd 31 1 Hz Lua manifest" },
        ],
        successComment: false,
        failComment: false,
        releasedLabels: false,
      },
    ],
  ],
};

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
          { path: "../release-assets/main.lua", label: "Ambyte field Lua script" },
          { path: "../release-assets/main.lua.manifest.json", label: "Lua release manifest" },
        ],
        successComment: false,
        failComment: false,
        releasedLabels: false,
      },
    ],
  ],
};

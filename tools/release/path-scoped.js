/**
 * Path-aware semantic-release hooks, following openJII's monorepo release
 * pattern. The repository has two independent release units without forcing a
 * disruptive firmware-directory move:
 *
 *   repository root -> firmware, every tracked path except lua/**
 *   lua/            -> field script, only lua/**
 *
 * Filtering both analysis and release-note steps matters. Merely skipping the
 * firmware job on a Lua-only merge is insufficient: the next firmware release
 * would otherwise re-read that Lua commit from history and bump firmware late.
 */
import { execFileSync } from "node:child_process";
import path from "node:path";
import { withFiles } from "semantic-release-monorepo/src/only-package-commits.js";
import { mapCommits } from "semantic-release-monorepo/src/options-transforms.js";
import { wrapStep } from "semantic-release-plugin-decorators";

const gitRoot = () =>
  execFileSync("git", ["rev-parse", "--show-toplevel"], { encoding: "utf8" }).trim();

const releaseUnit = () => {
  const relative = path.relative(gitRoot(), process.cwd());
  return relative === "lua" ? "lua" : "firmware";
};

const normalized = (file) => file.split(path.sep).join("/");

// Paths that belong to NO release unit: the desktop flash GUI is host-side
// tooling with its own build pipeline (flash-gui-build.yml) — a feat/fix there
// must not bump the firmware version any more than a Lua commit may.
const isUnreleased = (candidate) =>
  candidate === "flash_gui" || candidate.startsWith("flash_gui/");

export const isRelevantFile = (file, unit = releaseUnit()) => {
  const candidate = normalized(file);
  const isLua = candidate === "lua" || candidate.startsWith("lua/");
  return unit === "lua" ? isLua : !isLua && !isUnreleased(candidate);
};

export const onlyRelevantCommits = async (commits, unit = releaseUnit()) => {
  const commitsWithFiles = await withFiles(commits);
  return commitsWithFiles.filter(({ files }) => files.some((file) => isRelevantFile(file, unit)));
};

const withOnlyRelevantCommits = (plugin) => async (pluginConfig, config) =>
  plugin(pluginConfig, await mapCommits((commits) => onlyRelevantCommits(commits))(config));

const wrapperName = "ambyte-path-scoped-release";

export const analyzeCommits = wrapStep("analyzeCommits", withOnlyRelevantCommits, {
  wrapperName,
});
export const generateNotes = wrapStep(
  "generateNotes",
  withOnlyRelevantCommits,
  { wrapperName },
);
export const success = wrapStep(
  "success",
  withOnlyRelevantCommits,
  { wrapperName },
);
export const fail = wrapStep(
  "fail",
  withOnlyRelevantCommits,
  { wrapperName },
);

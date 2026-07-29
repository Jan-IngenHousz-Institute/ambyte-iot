export const RELEASE_TYPES = Object.freeze({
  feat: "minor",
  fix: "patch",
  perf: "patch",
  revert: "patch",
});

export const NO_RELEASE_TYPES = Object.freeze([
  "docs",
  "style",
  "chore",
  "refactor",
  "test",
  "build",
  "ci",
]);

export const TYPE_SECTIONS = Object.freeze({
  feat: "Features",
  fix: "Bug Fixes",
  perf: "Performance Improvements",
  revert: "Reverts",
  docs: "Documentation",
  style: "Styles",
  chore: "Miscellaneous Chores",
  refactor: "Code Refactoring",
  test: "Tests",
  build: "Build System",
  ci: "Continuous Integration",
});

export const APPROVED_BASELINE = Object.freeze({
  releaseTag: "v1.0.5",
  releaseSha: "508bca7c302c8a5e1b5214d5b03d243de6965ac6",
  commits: Object.freeze([
    Object.freeze({
      sha: "b98ef489614e9c6d98af70020f1adaaf36b1630b",
      parentSha: "508bca7c302c8a5e1b5214d5b03d243de6965ac6",
      subject: "rtc ota",
    }),
  ]),
});

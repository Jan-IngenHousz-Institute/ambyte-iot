// SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
// SPDX-License-Identifier: GPL-3.0-only

import assert from "node:assert/strict";
import test from "node:test";

import { firmwareBuildRequired } from "./pr-build-scope.js";

test("schedule-only PRs skip the firmware build", () => {
  assert.equal(firmwareBuildRequired(["schedule/default.yaml"]), false);
  assert.equal(
    firmwareBuildRequired(["schedule/default.yaml", "schedule/release.config.js"]),
    false,
  );
});

test("flash-GUI-only PRs skip the firmware build", () => {
  assert.equal(firmwareBuildRequired(["flash_gui/gui.py"]), false);
  assert.equal(
    firmwareBuildRequired([
      "flash_gui/gui.py",
      ".github/workflows/flash-gui-build.yml",
      "schedule/default.yaml",
    ]),
    false,
  );
});

test("any non-schedule path retains the firmware build gate", () => {
  assert.equal(
    firmwareBuildRequired(["schedule/default.yaml", "components/sched_runner/sched_runner.c"]),
    true,
  );
  assert.equal(firmwareBuildRequired([".github/workflows/pr.yml"]), true);
  assert.equal(firmwareBuildRequired(["components/old.c", "schedule/old.yaml"]), true);
  assert.equal(firmwareBuildRequired([]), true);
});

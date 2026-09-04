// SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
// SPDX-License-Identifier: GPL-3.0-only

import assert from "node:assert/strict";
import test from "node:test";

import { isRelevantFile } from "./path-scoped.js";

test("schedule release includes only schedule paths", () => {
  assert.equal(isRelevantFile("schedule/default.yaml", "schedule"), true);
  assert.equal(isRelevantFile("schedule/release.config.js", "schedule"), true);
  assert.equal(isRelevantFile("components/script_update/script_update.c", "schedule"), false);
});

test("firmware release excludes all schedule paths", () => {
  assert.equal(isRelevantFile("schedule/default.yaml", "firmware"), false);
  assert.equal(isRelevantFile("main/app_main.c", "firmware"), true);
  assert.equal(isRelevantFile("tools/build_schedule_release.py", "firmware"), true);
});

test("the flash GUI belongs to no release unit", () => {
  assert.equal(isRelevantFile("flash_gui/gui.py", "firmware"), false);
  assert.equal(isRelevantFile("flash_gui/vendor/nvs_partition_gen.py", "firmware"), false);
  assert.equal(
    isRelevantFile(".github/workflows/flash-gui-build.yml", "firmware"),
    false,
  );
  assert.equal(isRelevantFile("flash_gui/gui.py", "schedule"), false);
  // Not to be confused with on-device flash paths.
  assert.equal(isRelevantFile("components/ambit_flash/ambit_flash.c", "firmware"), true);
});

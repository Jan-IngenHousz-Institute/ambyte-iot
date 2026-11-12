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

test("host tests belong to no release unit, but production paths still do", () => {
  for (const file of ["tests", "tests/test_evq_storage.py", "tests/svc_host/runtime_driver.c"]) {
    assert.equal(isRelevantFile(file, "firmware"), false, file);
    assert.equal(isRelevantFile(file, "schedule"), false, file);
  }
  // The exclusion is anchored at the repository's host-test directory.
  assert.equal(isRelevantFile("tests-old/driver.c", "firmware"), true);
  assert.equal(isRelevantFile("components/event_log/tests/driver.c", "firmware"), true);
  assert.equal(isRelevantFile("components/event_log/event_log.c", "firmware"), true);
  assert.equal(isRelevantFile("tools/release/path-scoped.js", "firmware"), true);
});

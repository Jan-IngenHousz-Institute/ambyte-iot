// SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
// SPDX-License-Identifier: GPL-3.0-only

import assert from "node:assert/strict";
import test from "node:test";

import { firmwareBuildRequired } from "./pr-build-scope.js";

test("Lua-only PRs skip the firmware build", () => {
  assert.equal(firmwareBuildRequired(["lua/main.lua"]), false);
  assert.equal(
    firmwareBuildRequired(["lua/main.lua", "lua/release.config.js"]),
    false,
  );
});

test("flash-GUI-only PRs skip the firmware build", () => {
  assert.equal(firmwareBuildRequired(["flash_gui/gui.py"]), false);
  assert.equal(
    firmwareBuildRequired([
      "flash_gui/gui.py",
      ".github/workflows/flash-gui-build.yml",
      "lua/main.lua",
    ]),
    false,
  );
});

test("any non-Lua path retains the firmware build gate", () => {
  assert.equal(
    firmwareBuildRequired(["lua/main.lua", "components/lua_runner/lua_runner.c"]),
    true,
  );
  assert.equal(firmwareBuildRequired([".github/workflows/pr.yml"]), true);
  assert.equal(firmwareBuildRequired(["components/old.c", "lua/old.c"]), true);
  assert.equal(firmwareBuildRequired([]), true);
});

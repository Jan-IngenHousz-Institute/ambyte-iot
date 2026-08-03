import assert from "node:assert/strict";
import test from "node:test";

import { isRelevantFile } from "./path-scoped.js";

test("Lua release includes only lua paths", () => {
  assert.equal(isRelevantFile("lua/main.lua", "lua"), true);
  assert.equal(isRelevantFile("lua/release.config.js", "lua"), true);
  assert.equal(isRelevantFile("components/script_update/script_update.c", "lua"), false);
});

test("firmware release excludes all lua paths", () => {
  assert.equal(isRelevantFile("lua/main.lua", "firmware"), false);
  assert.equal(isRelevantFile("main/app_main.c", "firmware"), true);
  assert.equal(isRelevantFile("tools/build_lua_release.py", "firmware"), true);
});

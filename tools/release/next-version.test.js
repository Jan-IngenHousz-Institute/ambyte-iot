// SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
// SPDX-License-Identifier: GPL-3.0-only

import assert from "node:assert/strict";
import test from "node:test";

import { parseNextReleaseVersion } from "./next-version.js";

test("parses a release after an existing tag", () => {
  assert.equal(
    parseNextReleaseVersion("The next release version is 1.4.0"),
    "1.4.0",
  );
});

test("parses the first release for a new unit", () => {
  assert.equal(
    parseNextReleaseVersion(
      "There is no previous release, the next release version is 1.0.0",
    ),
    "1.0.0",
  );
});

test("uses the final semantic-release decision", () => {
  assert.equal(
    parseNextReleaseVersion(
      "The next release version is 1.4.0\nThe next release version is 1.4.1",
    ),
    "1.4.1",
  );
});

test("returns an empty value when no release is required", () => {
  assert.equal(parseNextReleaseVersion("There are no relevant changes"), "");
});

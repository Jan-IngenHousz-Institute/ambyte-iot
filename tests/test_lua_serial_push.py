# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Source-contract tests for the console serial-push script installer.

The invariants here are the reason the design is safe, and none of them are
visible from the Python side: the dangerous half of the install has exactly one
implementation, the local path deliberately leaves MQTT alone, and the staging
path is defined once.
"""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_UPDATE = (ROOT / "components/script_update/script_update.c").read_text()
SCRIPT_UPDATE_H = (
    ROOT / "components/script_update/include/script_update.h").read_text()
CLI = (ROOT / "components/CLI/CLI.c").read_text()
CLI_CMAKE = (ROOT / "components/CLI/CMakeLists.txt").read_text()


def code_only(text):
    """Strip C comments so a rule is never satisfied by prose about the rule."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def worker_body(name):
    body = SCRIPT_UPDATE.split(f"static void {name}(const script_req_t *r)\n{{", 1)[1]
    return body.split("\n}\n", 1)[0]


class SharedInstallTailTest(unittest.TestCase):
    def test_verify_and_swap_has_a_single_implementation(self):
        # Both workers must route through it, so the checksum/syntax/.bak/rename
        # sequence cannot drift between the URL and serial-push paths.
        self.assertEqual(
            SCRIPT_UPDATE.count("static bool verify_and_swap_staged("), 1)
        self.assertEqual(SCRIPT_UPDATE.count("verify_and_swap_staged(r, got, n"), 2)

    def test_neither_staged_file_worker_swaps_for_itself(self):
        tail = SCRIPT_UPDATE.split("static bool verify_and_swap_staged(", 1)[1]
        tail = tail.split("\n}\n", 1)[0]
        self.assertIn("rename(LUA_PATH, LUA_PATH_BAK)", tail)
        self.assertIn("rename(LUA_PATH_NEW, LUA_PATH)", tail)
        # Both staged-file workers must delegate rather than keep a copy.
        # (do_update_impl, the inline MQTT path, legitimately still has its own:
        # it hashes an in-memory string and drives lua_runner_stop/start directly
        # with a stop-timeout path instead of the workload hooks.)
        for name in ("do_update_url_impl", "do_update_local_impl"):
            self.assertNotIn("rename(", code_only(worker_body(name)), name)

    def test_every_failure_path_leaves_main_lua_alone(self):
        tail = SCRIPT_UPDATE.split("static bool verify_and_swap_staged(", 1)[1]
        tail = tail.split("\n}\n", 1)[0]
        mismatch = tail.index("sha256 mismatch")
        syntax = tail.index("syntax check failed")
        swap = tail.index("rename(LUA_PATH, LUA_PATH_BAK)")
        # Both rejections happen before anything touches the live script.
        self.assertLess(mismatch, swap)
        self.assertLess(syntax, swap)
        self.assertEqual(tail.count("remove(LUA_PATH_NEW)"), 2)


class LocalWorkerTest(unittest.TestCase):
    def _body(self):
        return worker_body("do_update_local_impl")

    def test_dispatched_from_the_worker(self):
        self.assertIn("#define OP_UPDATE_LOCAL 3", SCRIPT_UPDATE)
        self.assertIn("else if (r->op == OP_UPDATE_LOCAL) do_update_local(r);",
                      SCRIPT_UPDATE)

    def test_no_sd_gate_staging_is_internal(self):
        # Since main.lua + staging moved to internal littlefs, the local worker
        # must NOT take the SD RW-gate: a missing archive card cannot fail a
        # serial push. (The same de-gating applies to the other workers; the
        # whole component is SD-free.)
        wrapper = SCRIPT_UPDATE.split("static void do_update_local(const script_req_t *r)\n{", 1)[1]
        wrapper = wrapper.split("\n}\n", 1)[0]
        self.assertNotIn("sdcard_io_begin", wrapper)
        self.assertNotIn("sdcard_io_begin", SCRIPT_UPDATE)

    def test_stops_lua_but_never_touches_mqtt(self):
        body = self._body()
        # The runner must stop: the file it is executing gets swapped.
        self.assertIn("workload_suspend()", body)
        self.assertIn("workload_resume()", body)
        # Dropping MQTT exists to free TLS heap for a download. There is no
        # download here, and suspending it would reintroduce the reconnect wait.
        self.assertNotIn("comms_suspend", code_only(body))
        self.assertNotIn("comms_resume", code_only(body))

    def test_refuses_to_install_nothing(self):
        self.assertIn("no staged script (lua begin/put first)", self._body())

    def test_hashes_the_staged_file_rather_than_trusting_the_sender(self):
        self.assertIn("sha256_file(LUA_PATH_NEW, got)", self._body())


class ReconnectWaitTest(unittest.TestCase):
    def test_the_url_path_no_longer_waits_five_minutes_for_mqtt(self):
        # 3000 ticks at 100 ms is 300 s, which a board with no uplink always
        # spends in full, after the real work has already finished or failed.
        self.assertNotIn("i < 3000", SCRIPT_UPDATE)
        self.assertIn("#define SCRIPT_RECONNECT_WAIT_TICKS 150", SCRIPT_UPDATE)


class StagingPathTest(unittest.TestCase):
    def test_defined_once_and_shared_with_the_console(self):
        self.assertIn('#define SCRIPT_UPDATE_STAGING_PATH "/littlefs/main.lua.new"',
                      SCRIPT_UPDATE_H)
        self.assertIn("#define LUA_PATH_NEW   SCRIPT_UPDATE_STAGING_PATH",
                      SCRIPT_UPDATE)
        # The CLI must not hardcode its own copy of the path.
        self.assertNotIn('"/littlefs/main.lua.new"', CLI)
        self.assertIn("SCRIPT_UPDATE_STAGING_PATH", CLI)
        # Internal flash, not the archive card: a cardless board must install.
        self.assertNotIn('"/sdcard/main.lua.new"', SCRIPT_UPDATE_H)


class ConsoleCommandsTest(unittest.TestCase):
    def test_all_four_subcommands_exist_and_are_advertised(self):
        for sub in ("begin", "put", "commit", "abort"):
            self.assertIn(f'strcmp(argv[1], "{sub}") == 0', CLI)
        usage = CLI.split('printf("Usage: lua <', 1)[1].split(";", 1)[0]
        for sub in ("begin", "put", "commit", "abort"):
            self.assertIn(sub, usage)

    def test_put_is_bounded_and_base64_only(self):
        self.assertIn("mbedtls_base64_decode(", CLI)
        self.assertIn("LUA_PUT_TOTAL_MAX", CLI)
        self.assertIn("lua put: refusing to exceed", CLI)

    def test_put_keeps_no_state_between_commands(self):
        # A push abandoned halfway must leak no file handle, so each put opens,
        # appends, closes and re-stats. Staging is on internal littlefs, so no
        # SD gate may wrap it — a cardless board must accept a push.
        put = CLI.split('strcmp(argv[1], "put") == 0', 1)[1].split("return 0;", 1)[0]
        self.assertIn('fopen(SCRIPT_UPDATE_STAGING_PATH, "ab")', put)
        self.assertIn("fclose(f)", put)
        self.assertNotIn("sdcard_io_begin", put)

    def test_commit_mirrors_lua_install_and_does_not_reboot(self):
        commit = CLI.split('strcmp(argv[1], "commit") == 0', 1)[1].split("return 0;", 1)[0]
        # reboot=false: the GUI stays attached to verify, so an in-place restart
        # avoids another USB re-enumeration.
        self.assertIn("script_update_local_request(argv[2], argv[3], false,", commit)
        self.assertIn("isxdigit", commit)

    def test_component_dependencies_are_declared(self):
        for dep in ("sd_card", "mbedtls"):
            self.assertIn(dep, CLI_CMAKE)


if __name__ == "__main__":
    unittest.main()

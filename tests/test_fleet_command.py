"""fleet_command.py builds the right device payload and refuses unsafe replays."""
import argparse
import importlib
import pathlib
import subprocess
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools" / "fleet_deploy"
sys.path.insert(0, str(TOOLS))


def _load():
    try:
        return importlib.import_module("fleet_command")
    except ModuleNotFoundError as e:  # awscrt is not a host-test dependency
        raise unittest.SkipTest(f"fleet_command deps missing: {e}")


def _args(**kw):
    base = dict(cmd="ping", mode="status", token="", from_id=0, to_id=0, from_ms=0, to_ms=0, chunk=64)
    base.update(kw)
    return argparse.Namespace(**base)


class BuildMessage(unittest.TestCase):
    def setUp(self):
        self.fc = _load()

    def test_ping(self):
        self.assertEqual(self.fc.build_message(_args(), "r1"), {"type": "ping", "id": "r1"})

    def test_timezone_commands_are_explicit_and_nonempty(self):
        self.assertEqual(self.fc.build_message(_args(cmd="get-timezone"), "r"), {"type":"get_timezone","id":"r"})
        self.assertEqual(self.fc.build_message(_args(cmd="set-timezone", timezone="Australia/Darwin"), "r"),
                         {"type":"set_timezone","id":"r","timezone":"Australia/Darwin"})
        for zone in ("", "9.5", "Australia/Darwin\n", "bad zone", "x"*48):
            with self.subTest(zone=zone), self.assertRaises(SystemExit):
                self.fc.build_message(_args(cmd="set-timezone", timezone=zone), "r")

    def test_inventory_window_optional(self):
        self.assertEqual(self.fc.build_message(_args(cmd="inventory"), "r"),
                         {"type": "evlog_inventory", "id": "r", "list": True})
        msg = self.fc.build_message(_args(cmd="inventory", from_id=966, to_id=19559), "r")
        self.assertEqual((msg["from_id"], msg["to_id"]), (966, 19559))

    def test_replay_count_needs_token_and_window(self):
        with self.assertRaises(SystemExit):
            self.fc.build_message(_args(cmd="replay", mode="count", from_id=1, to_id=2), "r")
        with self.assertRaises(SystemExit):
            self.fc.build_message(_args(cmd="replay", mode="count", token="t"), "r")

    def test_replay_run_refuses_unbounded_window(self):
        with self.assertRaises(SystemExit):
            self.fc.build_message(_args(cmd="replay", mode="run", token="t", from_id=5), "r")
        msg = self.fc.build_message(_args(cmd="replay", mode="run", token="t", from_id=5, to_id=9, chunk=32), "r")
        self.assertEqual(msg, {"type": "evlog_replay", "id": "r", "mode": "run", "token": "t",
                               "from_id": 5, "to_id": 9, "chunk": 32})

    def test_status_and_cancel_carry_no_window(self):
        for mode in ("status", "cancel"):
            msg = self.fc.build_message(_args(cmd="replay", mode=mode, token="t", from_id=5, to_id=9), "r")
            self.assertNotIn("from_id", msg)
            self.assertEqual(msg["mode"], mode)

    def test_ambit_flash_with_manifest_url(self):
        url = "https://github.com/Jan-IngenHousz-Institute/ambit/releases/download/v1.4.0/manifest.json"
        msg = self.fc.build_message(_args(cmd="ambit-flash", version="1.4.0", channel="0", manifest_url=url), "r")
        self.assertEqual(msg, {"type": "ambit_flash", "id": "r", "version": "1.4.0", "channel": 0,
                               "manifest_url": url})
        msg = self.fc.build_message(_args(cmd="ambit-flash", version="1.4.0", channel="all", manifest_url=""), "r")
        self.assertEqual(msg, {"type": "ambit_flash", "id": "r", "version": "1.4.0", "channel": "all"})

    def test_ambit_flash_refuses_bad_input(self):
        for kw in (dict(version="1.4", channel="0", manifest_url=""),
                   dict(version="1.4.0", channel="4", manifest_url=""),
                   dict(version="1.4.0", channel="0", manifest_url="http://x/manifest.json"),
                   dict(version="1.4.0", channel="0", manifest_url="https://x/" + "a" * 250)):
            with self.subTest(**kw), self.assertRaises(SystemExit):
                self.fc.build_message(_args(cmd="ambit-flash", **kw), "r")

    def test_ambit_versions(self):
        self.assertEqual(self.fc.build_message(_args(cmd="ambit-versions"), "r"),
                         {"type": "ambit_versions", "id": "r"})

    def test_site_specific_timezone_refuses_discovery_before_aws_access(self):
        from unittest.mock import patch
        with patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("must not touch AWS")):
            with self.assertRaises(SystemExit) as error:
                self.fc.main(["set-timezone", "--timezone", "Australia/Darwin"])
        self.assertEqual(error.exception.code, 2)

    def test_timezone_duplicate_busy_waits_for_the_actual_applied_reply(self):
        import json
        from types import SimpleNamespace
        from unittest.mock import patch
        client = "ambyte_14:63:93:37:A1:E8"
        future = SimpleNamespace(result=lambda **kw: None)
        def connection(session, subscription, receive, **kwargs):
            def publish(*, topic, payload, qos):
                request=json.loads(payload)
                base={"id":request["id"],"type":"set_timezone_result"}
                status=f"experiment/data_ingest/v1/experiment/ambyte/v1.0/{client}/status"
                receive(status,json.dumps(dict(base,ok=False,detail="busy")).encode())
                receive(status,json.dumps(dict(base,ok=True,saved=True,applied=True,
                    timezone="Australia/Darwin",active_timezone="Australia/Darwin",utc_offset_seconds=34200,detail="applied")).encode())
                return future, 1
            return SimpleNamespace(publish=publish,disconnect=lambda:future)
        aws=SimpleNamespace(mqtt=SimpleNamespace(QoS=SimpleNamespace(AT_LEAST_ONCE=1)))
        with patch.dict(sys.modules,awscrt=aws), patch.object(self.fc.fleet_deploy,"boto_session",return_value=object()), \
             patch.object(self.fc.fleet_deploy,"mqtt_connection",connection), patch.object(self.fc.time,"sleep",lambda _:None):
            result=self.fc.main(["set-timezone","--devices",client,"--timezone","Australia/Darwin","--expect-offset","34200","--wait","1"])
        self.assertEqual(result,0)

    def test_devices_keep_case(self):
        devs = self.fc.parse_devices("ambyte_28:37:2F:FF:FC:80, 28:37:2F:FF:E7:50")
        self.assertIn("ambyte_28:37:2F:FF:FC:80", devs)
        self.assertIn("AMBYTE_28:37:2F:FF:E7:50", devs)


class LogCommands(unittest.TestCase):
    """log-list / log-read: payloads, the chunk stitcher and the mocked transport."""

    CLIENT = "ambyte_14:63:93:37:A1:E8"
    STATUS = f"experiment/data_ingest/v1/experiment/ambyte/v1.0/{CLIENT}/status"

    def setUp(self):
        self.fc = _load()

    def test_log_payloads(self):
        self.assertEqual(self.fc.build_message(_args(cmd="log-list"), "r"), {"type": "log_list", "id": "r"})
        msg = self.fc.build_message(_args(cmd="log-read", name="ambyte.log", offset=0, length=0, tail=0, out=None), "r")
        self.assertEqual(msg, {"type": "log_read", "id": "r", "name": "ambyte.log", "offset": 0, "length": 4096})
        msg = self.fc.build_message(_args(cmd="log-read", name="ambyte.1.log", offset=0, length=0, tail=65536, out="x"), "r")
        self.assertEqual((msg["offset"], msg["length"]), (-65536, 8192))
        msg = self.fc.build_message(_args(cmd="log-read", name="ambyte.log", offset=-10, length=100, tail=0, out=None), "r")
        self.assertEqual((msg["offset"], msg["length"]), (-10, 100))

    def test_log_read_refuses_bad_names_and_windows(self):
        for name in ("", "../secret", "/sdcard/events/ev-000001.log", "a/b", "..", ".hidden", "x" * 65, "we ird"):
            with self.subTest(name=name), self.assertRaises(SystemExit):
                self.fc.build_message(_args(cmd="log-read", name=name, offset=0, length=0, tail=0, out=None), "r")
        for kw in (dict(length=8193), dict(length=-1), dict(tail=-5), dict(tail=10, offset=5)):
            base = dict(cmd="log-read", name="ambyte.log", offset=0, length=0, tail=0, out=None)
            base.update(kw)
            with self.subTest(**kw), self.assertRaises(SystemExit):
                self.fc.build_message(_args(**base), "r")

    def test_bad_log_read_never_touches_aws(self):
        from unittest.mock import patch
        cases = (["log-read", "--devices", self.CLIENT, "--name", "../x"],
                 ["log-read", "--devices", self.CLIENT, "--name", "ambyte.log", "--length", "9000"],
                 ["log-read", "--name", "ambyte.log", "--out", "f"],            # --out needs one explicit device
                 ["log-read", "--devices", f"{self.CLIENT},28:37:2F:FF:E7:50", "--name", "ambyte.log", "--out", "f"])
        with patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("must not touch AWS")):
            for argv in cases:
                with self.subTest(argv=argv), self.assertRaises(SystemExit) as err:
                    self.fc.main(argv)
                self.assertEqual(err.exception.code, 2)

    @staticmethod
    def chunk(name, data, offset, size, eof, gen="7.1.4", **extra):
        import base64
        reply = {"type": "log_read_result", "ok": True, "detail": "ok", "name": name, "gen": gen, "offset": offset,
                 "length": len(data), "size": size, "eof": eof, "encoding": "base64",
                 "data": base64.b64encode(data).decode()}
        reply.update(extra)
        return reply

    def test_stitcher_tail_chunks_and_consistency_checks(self):
        st = self.fc.LogStitcher("ambyte.log", -5000, 4096)
        self.assertEqual(st.request("i")["offset"], -5000)
        self.assertEqual(st.feed(self.chunk("ambyte.log", b"a" * 4096, 15000, 20000, False)), "continue")
        self.assertEqual(st.request("i")["offset"], 19096)       # absolute after the device resolved the tail
        self.assertEqual(st.feed(self.chunk("ambyte.log", b"b" * 904, 19096, 20000, True)), "done")
        self.assertEqual((bytes(st.data), st.start, st.size), (b"a" * 4096 + b"b" * 904, 15000, 20000))
        bad = [
            (self.chunk("ambyte.1.log", b"x", 0, 1, True), "asked for"),            # other file
            (self.chunk("ambyte.log", b"x", 7, 8, True), "offset 7"),                # wrong offset
            (dict(self.chunk("ambyte.log", b"xy", 0, 9, True), length=3), "decodes"),  # length mismatch
            (dict(self.chunk("ambyte.log", b"x", 0, 9, True), data="!!"), "malformed"),
            ({"type": "log_read_result", "ok": False, "detail": "sd_parked"}, "sd_parked"),
        ]
        for reply, why in bad:
            st = self.fc.LogStitcher("ambyte.log", 0, 4096)
            with self.subTest(why=why):
                self.assertEqual(st.feed(reply), "error")
                self.assertIn(why, st.error)
        st = self.fc.LogStitcher("ambyte.log", 0, 4)
        st.feed(self.chunk("ambyte.log", b"abcd", 0, 100, False))
        self.assertEqual(st.feed(self.chunk("ambyte.log", b"e", 4, 5, True)), "error")   # rotated under us
        self.assertIn("shrank", st.error)

    def test_stitcher_pins_gen_and_refuses_same_size_replacement(self):
        # Review finding 5: AAAA from one file, BBBB from its same-size
        # successor under the same basename. The second chunk carries another
        # gen; it must never be spliced on.
        st = self.fc.LogStitcher("ambyte.1.log", 0, 4)
        self.assertNotIn("expect_gen", st.request("i"))
        self.assertEqual(st.feed(self.chunk("ambyte.1.log", b"AAAA", 0, 8, False, gen="7.1.4")), "continue")
        self.assertEqual(st.request("i")["expect_gen"], "7.1.4")
        self.assertEqual(st.feed(self.chunk("ambyte.1.log", b"BBBB", 4, 8, True, gen="7.1.6")), "error")
        self.assertIn("changed identity", st.error)
        self.assertFalse(st.done)

    def test_stitcher_restarts_once_on_file_rotated(self):
        st = self.fc.LogStitcher("ambyte.1.log", -8, 4)
        st.feed(self.chunk("ambyte.1.log", b"AAAA", 0, 8, False, gen="7.1.4"))
        self.assertEqual(st.feed({"type": "log_read_result", "ok": False, "detail": "file_rotated",
                                  "name": "ambyte.1.log", "gen": "7.1.6"}), "restart")
        req = st.request("i")
        self.assertEqual((req["offset"], "expect_gen" in req, bytes(st.data)), (-8, False, b""))
        st.feed(self.chunk("ambyte.1.log", b"BBBB", 0, 8, False, gen="7.1.6"))
        self.assertEqual(st.feed({"ok": False, "detail": "file_rotated"}), "error")   # only once
        self.assertIn("file_rotated", st.error)
        st = self.fc.LogStitcher("ambyte.log", 0, 4)
        self.assertEqual(st.feed({"ok": False, "detail": "rotation_in_progress"}), "retry")

    def test_incomplete_success_replies_are_rejected(self):
        # Review finding 4: what a device out of memory mid-reply produced.
        bad_lists = [
            {"type": "log_list_result", "ok": True, "gen": "1.1.0"},
            {"type": "log_list_result", "ok": True, "gen": "1.1.0", "files": []},
            {"type": "log_list_result", "ok": True, "gen": "1.1.0", "files": [{"size": 5}], "omitted": 0},
            {"type": "log_list_result", "ok": True, "gen": "1.1.0", "files": [{"name": "a.log"}], "omitted": 0},
            {"type": "log_list_result", "ok": True, "files": [], "omitted": 0},
        ]
        for reply in bad_lists:
            with self.subTest(reply=reply):
                self.assertIsNotNone(self.fc.log_reply_problem(reply))
        self.assertIsNone(self.fc.log_reply_problem({"type": "log_list_result", "ok": True, "gen": "1.1.0",
                                                     "files": [{"name": "a.log", "size": 5}], "omitted": 0}))
        good = self.chunk("ambyte.log", b"hello", 0, 5, True)
        self.assertIsNone(self.fc.log_reply_problem(good))
        for k in ("data", "length", "eof", "encoding", "name", "size", "gen"):
            with self.subTest(missing=k):
                self.assertIsNotNone(self.fc.log_reply_problem({x: v for x, v in good.items() if x != k}))
        self.assertIsNotNone(self.fc.log_reply_problem(dict(good, length=4)))
        self.assertIsNotNone(self.fc.log_reply_problem(dict(good, eof=False)))
        rc, _ = self.run_main(["log-list", "--devices", self.CLIENT, "--wait", "1"], lambda req: [bad_lists[2]])
        self.assertEqual(rc, 1)
        rc, _ = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log", "--wait", "1"],
                              lambda req: [dict(good, data="aGVs")])
        self.assertEqual(rc, 1)

    def test_out_dry_run_publishes_nothing_and_writes_nothing(self):
        # Review finding 1: --out used to dispatch to the stitcher before the
        # dry-run check and published for real.
        import io
        import os
        import tempfile
        from contextlib import redirect_stdout
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("no AWS in a dry run")), \
             patch.object(self.fc.fleet_deploy, "mqtt_connection", side_effect=AssertionError("no MQTT in a dry run")):
            out = io.StringIO()
            with redirect_stdout(out):
                rc = self.fc.main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log", "--tail", "65536",
                                   "--out", f"{tmp}/device-log.txt", "--dry-run"])
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(f"{tmp}/device-log.txt"))
        text = out.getvalue()
        self.assertIn(f"target={self.CLIENT}", text)
        self.assertIn('"offset": -65536', text)
        self.assertIn("nothing published", text)

    def test_workflow_dry_run_reaches_the_cli_as_dry_run(self):
        # The workflow always adds --out for log-read; with dry_run=true the
        # exact argv it builds must run the CLI's no-publish path.
        import os
        import pathlib
        import shlex
        import subprocess
        import tempfile
        import yaml
        root = pathlib.Path(__file__).resolve().parents[1]
        wf = yaml.safe_load((root / ".github/workflows/fleet-command.yml").read_text())
        step = next(s for s in wf["jobs"]["command"]["steps"] if s.get("id") == "run")
        script = step["run"].replace("${{ inputs.environment }}", "dev")
        with tempfile.TemporaryDirectory() as tmp:
            fake = pathlib.Path(tmp) / "python"
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGV_OUT"\n')
            fake.chmod(0o755)
            env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}", ARGV_OUT=f"{tmp}/argv",
                       GITHUB_STEP_SUMMARY=f"{tmp}/summary", COMMAND="log-read", DEVICES=self.CLIENT,
                       TIMEZONE="", EXPECT_OFFSET="", WINDOW="1440", EXPECT_FW="", MODE="status", TOKEN="",
                       FROM_ID="0", TO_ID="0", FROM_MS="0", TO_MS="0", CHUNK="64", WAIT="90", DRY_RUN="true",
                       LOG_NAME="ambyte.log", LOG_TAIL="65536")
            r = subprocess.run(["bash", "-c", script], cwd=tmp, env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            argv = pathlib.Path(f"{tmp}/argv").read_text().splitlines()
        self.assertEqual(argv[0], "tools/fleet_deploy/fleet_command.py")
        argv = argv[1:]
        self.assertIn("--dry-run", argv)
        self.assertIn("--out", argv)
        from unittest.mock import patch
        with patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("no AWS in a dry run")), \
             patch.object(self.fc.fleet_deploy, "mqtt_connection", side_effect=AssertionError("no MQTT in a dry run")), \
             tempfile.TemporaryDirectory() as tmp:
            cwd = os.getcwd()
            os.chdir(tmp)
            try:
                rc = self.fc.main(argv)
            finally:
                os.chdir(cwd)
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(f"{tmp}/device-log.txt"))
        self.assertTrue(shlex.join(argv))

    def test_log_read_out_restarts_on_rotation_and_fails_on_a_second(self):
        import tempfile

        def make_device(rotations):
            """Same-size replacement of the file after each first chunk, up to `rotations` times."""
            state = {"n": 0, "left": rotations, "served": 0}
            content = lambda: bytes([ord("A") + state["n"]]) * 9000
            gen = lambda: f"7.1.{4 + 2 * state['n']}"
            def device(req):
                if "expect_gen" in req and req["expect_gen"] != gen():
                    yield {"type": "log_read_result", "ok": False, "detail": "file_rotated",
                           "name": req["name"], "gen": gen()}
                    state["served"] = 0
                    return
                c = content()
                start = req["offset"] if req["offset"] >= 0 else max(0, len(c) + req["offset"])
                data = c[start:start + req["length"]]
                yield self.chunk(req["name"], data, start, len(c), start + len(data) >= len(c), gen=gen())
                state["served"] += 1
                if state["served"] == 1 and state["left"] > 0:
                    state["left"] -= 1
                    state["n"] += 1
            return device

        with tempfile.TemporaryDirectory() as tmp:
            rc, sent = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.1.log",
                                      "--out", f"{tmp}/f", "--wait", "1"], make_device(1))
            self.assertEqual(rc, 0)
            with open(f"{tmp}/f", "rb") as fh:
                self.assertEqual(fh.read(), b"B" * 9000)          # never AAAA + BBBB
        self.assertEqual([r.get("expect_gen") for r in sent], [None, "7.1.4", None, "7.1.6"])
        with tempfile.TemporaryDirectory() as tmp:
            rc, _ = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.1.log",
                                   "--out", f"{tmp}/f", "--wait", "1"], make_device(2))
        self.assertEqual(rc, 1)                                  # restarts only once

    def test_empty_chunk_before_eof_never_completes(self):
        """Review round 2, finding 5."""
        import tempfile
        empty = {"type": "log_read_result", "ok": True, "detail": "ok", "name": "ambyte.log", "gen": "7.1.4",
                 "size": 100, "offset": 0, "length": 0, "eof": False, "encoding": "base64", "data": ""}
        self.assertIn("no progress", self.fc.log_reply_problem(empty))
        st = self.fc.LogStitcher("ambyte.log", 0, 4096)
        self.assertEqual(st.feed(empty), "error")
        self.assertFalse(st.done)
        with tempfile.TemporaryDirectory() as tmp:
            res = f"{tmp}/r.json"
            rc, _ = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log", "--out", f"{tmp}/f",
                                   "--wait", "1", "--results-json", res], lambda req: [empty])
            self.assertEqual(rc, 1)
            import json
            with open(res) as fh:
                self.assertFalse(json.load(fh)["log"]["complete"])
        # Exact EOF (length 0, eof true) still completes.
        st = self.fc.LogStitcher("ambyte.log", 100, 4096)
        self.assertEqual(st.feed(dict(empty, offset=100, eof=True)), "done")

    def test_explicit_expect_gen_is_sent_and_never_traded(self):
        """Review round 2, finding 3: --expect-gen with --out."""
        import io
        import tempfile
        from contextlib import redirect_stdout
        honours = lambda req: ([{"type": "log_read_result", "ok": False, "detail": "file_rotated",
                                 "name": req["name"], "gen": "8.1.0"}]
                               if req.get("expect_gen") not in (None, "8.1.0") else
                               [self.chunk(req["name"], b"x" * 10, 0, 10, True, gen="8.1.0")])
        ignores = lambda req: [self.chunk(req["name"], b"x" * 10, 0, 10, True, gen="8.1.0")]
        for label, device in (("device refuses", honours), ("device ignores expect_gen", ignores)):
            with self.subTest(label), tempfile.TemporaryDirectory() as tmp:
                out = io.StringIO()
                with redirect_stdout(out):
                    rc, sent = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log",
                                              "--expect-gen", "7.1.0", "--out", f"{tmp}/f", "--wait", "1"], device)
                self.assertEqual(rc, 1, out.getvalue())
                self.assertEqual(sent[0]["expect_gen"], "7.1.0")           # first request carries it
                self.assertTrue(all(r.get("expect_gen") == "7.1.0" for r in sent))   # never restarted onto 8.1.0
                with open(f"{tmp}/f", "rb") as fh:
                    self.assertEqual(fh.read(), b"")
        st = self.fc.LogStitcher("ambyte.log", 0, 4, expect_gen="7.1.0")
        self.assertEqual(st.feed({"ok": False, "detail": "file_rotated"}), "error")
        # The dry-run plan shows it too.
        from unittest.mock import patch
        out = io.StringIO()
        with patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("no AWS")), \
             redirect_stdout(out), tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.fc.main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log",
                                           "--expect-gen", "7.1.0", "--out", f"{tmp}/f", "--dry-run"]), 0)
        self.assertIn('"expect_gen": "7.1.0"', out.getvalue())
        self.assertIn("require gen 7.1.0", out.getvalue())
        # A matching explicit gen reads normally.
        with tempfile.TemporaryDirectory() as tmp:
            rc, _ = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log",
                                   "--expect-gen", "8.1.0", "--out", f"{tmp}/f", "--wait", "1"], honours)
            self.assertEqual(rc, 0)

    def test_stitch_across_a_reboot_without_boot_counter(self):
        """Review round 2, finding 4: boot_count 0 on both boots; the nonce differs."""
        import tempfile
        state = {"gen": "0.1111aaaa.1.0", "data": b"A" * 9000, "served": 0}
        def device(req):
            if "expect_gen" in req and req["expect_gen"] != state["gen"]:
                yield {"type": "log_read_result", "ok": False, "detail": "file_rotated",
                       "name": req["name"], "gen": state["gen"]}
                return
            c = state["data"]
            start = req["offset"] if req["offset"] >= 0 else max(0, len(c) + req["offset"])
            data = c[start:start + req["length"]]
            yield self.chunk(req["name"], data, start, len(c), start + len(data) >= len(c), gen=state["gen"])
            state["served"] += 1
            if state["served"] == 1:                        # reboot: same size, other bytes
                state.update(gen="0.2222bbbb.1.0", data=b"B" * 9000)
        with tempfile.TemporaryDirectory() as tmp:
            rc, sent = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.1.log",
                                      "--out", f"{tmp}/f", "--wait", "1"], device)
            self.assertEqual(rc, 0)
            with open(f"{tmp}/f", "rb") as fh:
                self.assertEqual(fh.read(), b"B" * 9000)
        self.assertEqual(sent[1]["expect_gen"], "0.1111aaaa.1.0")

    def test_stitcher_backs_off_on_no_mem_and_retries_busy(self):
        st = self.fc.LogStitcher("ambyte.log", 0, 8192)
        self.assertEqual(st.feed({"ok": False, "detail": "no_mem"}), "retry")
        self.assertEqual(st.request("i")["length"], 4096)
        self.assertEqual(st.feed({"ok": False, "detail": "busy"}), "retry")
        self.assertEqual(st.request("i")["offset"], 0)

    def fake_transport(self, handler):
        import json
        from types import SimpleNamespace
        future = SimpleNamespace(result=lambda **kw: None)
        sent = []
        def connection(session, subscription, receive, **kwargs):
            def publish(*, topic, payload, qos):
                request = json.loads(payload)
                sent.append(request)
                for reply in handler(request):
                    receive(self.STATUS, json.dumps({"id": request["id"], **reply}).encode())
                return future, 1
            return SimpleNamespace(publish=publish, disconnect=lambda: future)
        aws = SimpleNamespace(mqtt=SimpleNamespace(QoS=SimpleNamespace(AT_LEAST_ONCE=1)))
        return connection, aws, sent

    def run_main(self, argv, handler):
        from unittest.mock import patch
        connection, aws, sent = self.fake_transport(handler)
        with patch.dict(sys.modules, awscrt=aws), patch.object(self.fc.fleet_deploy, "boto_session", return_value=object()), \
             patch.object(self.fc.fleet_deploy, "mqtt_connection", connection), patch.object(self.fc.time, "sleep", lambda _: None):
            return self.fc.main(argv), sent

    def test_log_read_out_stitches_until_eof_with_correlated_ids(self):
        import json
        import tempfile
        content = bytes((i * 31) % 256 for i in range(20000))
        state = {"n": 0}
        def device(req):
            self.assertEqual(req["type"], "log_read")
            state["n"] += 1
            if state["n"] == 2:                          # a QoS duplicate refusal first, then the answer
                yield {"type": "log_read_result", "ok": False, "detail": "busy", "name": req["name"]}
            start = max(0, len(content) + req["offset"]) if req["offset"] < 0 else req["offset"]
            data = content[start:start + req["length"]]
            yield {"type": "log_read_result", "id": "other", "ok": True}   # uncorrelated: ignored
            yield self.chunk(req["name"], data, start, len(content), start + len(data) >= len(content))
        with tempfile.TemporaryDirectory() as tmp:
            out, res = f"{tmp}/ambyte.log", f"{tmp}/results.json"
            rc, sent = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log", "--tail", "18000",
                                      "--out", out, "--wait", "1", "--results-json", res], device)
            self.assertEqual(rc, 0)
            with open(out, "rb") as fh:
                self.assertEqual(fh.read(), content[-18000:])
            with open(res) as fh:
                results = json.load(fh)
        self.assertTrue(results["log"]["complete"])
        self.assertEqual(results["log"]["chunks"], 3)
        self.assertNotIn("data", json.dumps(results))      # bytes live in --out only
        self.assertEqual([r["offset"] for r in sent], [-18000, 10192, 18384])
        self.assertEqual(len({r["id"] for r in sent}), 3)

    def test_log_read_out_missing_reply_fails_and_keeps_partial(self):
        import tempfile
        def device(req):
            if req["offset"] == 0:
                yield self.chunk("ambyte.log", b"x" * 8192, 0, 9000, False)
        with tempfile.TemporaryDirectory() as tmp:
            rc, _ = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log",
                                   "--out", f"{tmp}/f", "--wait", "1"], device)
            self.assertEqual(rc, 1)
            with open(f"{tmp}/f", "rb") as fh:
                self.assertEqual(len(fh.read()), 8192)

    def test_log_list_and_single_read_results(self):
        import json
        import tempfile
        listing = {"type": "log_list_result", "ok": True, "detail": "ok", "dir": "/sdcard/logs", "gen": "7.1.4",
                   "files": [{"name": "ambyte.log", "size": 12}], "omitted": 0}
        with tempfile.TemporaryDirectory() as tmp:
            rc, sent = self.run_main(["log-list", "--devices", self.CLIENT, "--wait", "1",
                                      "--results-json", f"{tmp}/r.json"], lambda req: [listing])
            self.assertEqual(rc, 0)
            self.assertEqual(sent[0]["type"], "log_list")
            with open(f"{tmp}/r.json") as fh:
                self.assertEqual(json.load(fh)["replies"][self.CLIENT]["files"], listing["files"])
        refused = {"type": "log_list_result", "ok": False, "detail": "sd_parked", "dir": "/sdcard/logs"}
        rc, _ = self.run_main(["log-list", "--devices", self.CLIENT, "--wait", "1"], lambda req: [refused])
        self.assertEqual(rc, 1)                                 # a refusal fails the command
        rc, _ = self.run_main(["log-list", "--devices", self.CLIENT, "--wait", "1"], lambda req: [])
        self.assertEqual(rc, 1)                                 # a missing reply fails the command
        busy_then_ok = lambda req: [{"type": "log_read_result", "ok": False, "detail": "busy"},
                                    self.chunk("ambyte.log", b"hello", 0, 5, True)]
        rc, sent = self.run_main(["log-read", "--devices", self.CLIENT, "--name", "ambyte.log", "--wait", "1"],
                                 busy_then_ok)
        self.assertEqual(rc, 0)
        self.assertEqual(sent[0], {"type": "log_read", "id": sent[0]["id"], "name": "ambyte.log",
                                   "offset": 0, "length": 4096})


class OptionalAwsDependencies(unittest.TestCase):
    def test_command_and_bench_helpers_import_without_aws_packages(self):
        # Fresh interpreter: an earlier test's cached SDK import cannot hide
        # an eager dependency. These pure/mocked paths never open a transport.
        script = """
import argparse
import sys
sys.modules.update(awscrt=None, awsiot=None, boto3=None)
sys.path.insert(0, sys.argv[1])
import fleet_command
from tools import bench_signed_ota
assert fleet_command.build_message(argparse.Namespace(cmd='ping'), 'id') == {'type': 'ping', 'id': 'id'}
assert bench_signed_ota.exact_url('https://example.test/app') == 'https://example.test/app'
"""
        result = subprocess.run([sys.executable, "-c", script, str(TOOLS)],
                                cwd=TOOLS.parents[1], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()


class PowerProtectCommand(unittest.TestCase):
    """power-protect: payloads, refusals before AWS, MUTATING marker, verification."""

    CLIENT = "ambyte_14:63:93:39:36:48"
    STATUS = f"experiment/data_ingest/v1/experiment/ambyte/v1.0/{CLIENT}/status"

    def setUp(self):
        self.fc = _load()

    def test_payloads(self):
        b = self.fc.build_message
        self.assertEqual(b(_args(cmd="power-protect", action="status", state=""), "r"),
                         {"type": "power_protect", "id": "r", "action": "status"})
        self.assertEqual(b(_args(cmd="power-protect", action="clear", state=""), "r"),
                         {"type": "power_protect", "id": "r", "action": "clear"})
        for state in ("no_flash", "hold"):
            self.assertEqual(b(_args(cmd="power-protect", action="force", state=state), "r"),
                             {"type": "power_protect", "id": "r", "action": "force", "state": state})

    def test_bad_input_refused(self):
        for kw in (dict(action="force", state=""), dict(action="force", state="normal"),
                   dict(action="status", state="hold"), dict(action="clear", state="no_flash"),
                   dict(action="reboot", state="")):
            with self.subTest(**kw), self.assertRaises(SystemExit):
                self.fc.build_message(_args(cmd="power-protect", **kw), "r")

    def test_mutating_actions_refuse_discovery_and_bad_input_before_aws(self):
        from unittest.mock import patch
        cases = (["power-protect", "--action", "clear"],
                 ["power-protect", "--action", "force", "--state", "hold"],
                 ["power-protect", "--action", "force", "--devices", self.CLIENT],
                 ["power-protect", "--action", "status", "--state", "hold", "--devices", self.CLIENT],
                 ["power-protect", "--action", "force", "--state", "normal", "--devices", self.CLIENT])
        for argv in cases:
            with self.subTest(argv=argv), \
                 patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("must not touch AWS")), \
                 self.assertRaises(SystemExit) as error:
                self.fc.main(argv)
            self.assertEqual(error.exception.code, 2)

    def _run(self, argv, reply_for):
        import io
        import json
        from contextlib import redirect_stdout
        from types import SimpleNamespace
        from unittest.mock import patch
        future = SimpleNamespace(result=lambda **kw: None)
        sent = []

        def connection(session, subscription, receive, **kwargs):
            def publish(*, topic, payload, qos):
                request = json.loads(payload)
                sent.append(request)
                for reply in reply_for(request):
                    receive(self.STATUS, json.dumps(dict(reply, id=request["id"],
                                                         type="power_protect_result")).encode())
                return future, 1
            return SimpleNamespace(publish=publish, disconnect=lambda: future)
        aws = SimpleNamespace(mqtt=SimpleNamespace(QoS=SimpleNamespace(AT_LEAST_ONCE=1)))
        out = io.StringIO()
        with patch.dict(sys.modules, awscrt=aws), \
             patch.object(self.fc.fleet_deploy, "boto_session", return_value=object()), \
             patch.object(self.fc.fleet_deploy, "mqtt_connection", connection), \
             patch.object(self.fc.time, "sleep", lambda _: None), redirect_stdout(out):
            rc = self.fc.main([*argv, "--devices", self.CLIENT, "--wait", "1"])
        return rc, sent, out.getvalue()

    def test_status_is_read_only_and_force_is_mutating_and_verified(self):
        status = dict(ok=True, action="status", detail="ok", state="no_flash", reason="brownout",
                      forced=False, since_utc=1763152200, brownouts_6h=1, exit_in_s=None,
                      skipped={"flash_runs": 3, "ambit_runs": 0, "ota": 1})
        rc, sent, text = self._run(["power-protect"], lambda req: [status])
        self.assertEqual(rc, 0)
        self.assertEqual(sent[0]["action"], "status")
        self.assertNotIn("MUTATING", text)
        self.assertIn("state=no_flash reason=brownout", text)

        forced = dict(status, action="force", detail="applied", state="hold", reason="forced", forced=True)
        busy = dict(ok=False, action="force", detail="busy", state="no_flash")
        rc, sent, text = self._run(["power-protect", "--action", "force", "--state", "hold"],
                                   lambda req: [busy, forced])     # a QoS1 duplicate first
        self.assertEqual(rc, 0, text)
        self.assertEqual(sent[0], {"type": "power_protect", "id": sent[0]["id"], "action": "force", "state": "hold"})
        self.assertIn("(MUTATING)", text)

        cleared = dict(status, action="clear", detail="cleared", state="normal", reason="cleared", forced=False)
        rc, _, text = self._run(["power-protect", "--action", "clear"], lambda req: [cleared])
        self.assertEqual(rc, 0, text)
        self.assertIn("(MUTATING)", text)

    def test_refusal_or_unconfirmed_state_fails_the_run(self):
        refused = dict(ok=False, action="force", detail="maintenance_active", state="normal")
        rc, _, text = self._run(["power-protect", "--action", "force", "--state", "no_flash"], lambda req: [refused])
        self.assertEqual(rc, 1)
        self.assertIn("not confirmed", text)
        wrong = dict(ok=True, action="force", detail="applied", state="hold", forced=True)
        rc, _, _ = self._run(["power-protect", "--action", "force", "--state", "no_flash"], lambda req: [wrong])
        self.assertEqual(rc, 1)
        persist = dict(ok=False, action="clear", detail="persist_failed", state="normal", forced=False)
        rc, _, _ = self._run(["power-protect", "--action", "clear"], lambda req: [persist])
        self.assertEqual(rc, 1)

    def test_workflow_offers_power_protect_and_passes_action_and_state(self):
        import os
        import pathlib
        import subprocess
        import tempfile
        import yaml
        root = pathlib.Path(__file__).resolve().parents[1]
        wf = yaml.safe_load((root / ".github/workflows/fleet-command.yml").read_text())
        inputs = wf[True]["workflow_dispatch"]["inputs"]
        self.assertIn("power-protect", inputs["command"]["options"])
        self.assertEqual(inputs["power_protect_action"]["options"], ["status", "clear", "force"])
        self.assertEqual(inputs["power_protect_state"]["options"], ["no_flash", "hold"])
        self.assertLessEqual(len(inputs), 25)    # GitHub's workflow_dispatch input limit
        step = next(s for s in wf["jobs"]["command"]["steps"] if s.get("id") == "run")
        script = step["run"].replace("${{ inputs.environment }}", "dev")

        def argv_for(action, state, devices):
            with tempfile.TemporaryDirectory() as tmp:
                fake = pathlib.Path(tmp) / "python"
                fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGV_OUT"\n')
                fake.chmod(0o755)
                env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}", ARGV_OUT=f"{tmp}/argv",
                           GITHUB_STEP_SUMMARY=f"{tmp}/summary", COMMAND="power-protect", DEVICES=devices,
                           TIMEZONE="", EXPECT_OFFSET="", WINDOW="1440", EXPECT_FW="", MODE="status", TOKEN="",
                           FROM_ID="0", TO_ID="0", FROM_MS="0", TO_MS="0", CHUNK="64", WAIT="90",
                           DRY_RUN="false", LOG_NAME="", LOG_TAIL="65536", PP_ACTION=action, PP_STATE=state)
                r = subprocess.run(["bash", "-c", script], cwd=tmp, env=env, capture_output=True, text=True)
                argv = pathlib.Path(f"{tmp}/argv").read_text().splitlines() if r.returncode == 0 else None
                return r.returncode, argv

        rc, argv = argv_for("force", "hold", self.CLIENT)
        self.assertEqual(rc, 0)
        self.assertEqual(argv[1], "power-protect")
        self.assertEqual(argv[argv.index("--action") + 1], "force")
        self.assertEqual(argv[argv.index("--state") + 1], "hold")
        rc, argv = argv_for("status", "hold", "")
        self.assertEqual(rc, 0)
        self.assertNotIn("--state", argv)       # the state input only rides with force
        rc, _ = argv_for("clear", "no_flash", " ")
        self.assertEqual(rc, 2)                 # mutating without explicit devices


class VinLimit(unittest.TestCase):
    """vin-limit: get is read-only; set is explicit, validated and verified by readback."""
    CLIENT = "ambyte_28:37:2F:FF:FC:80"
    STATUS = f"experiment/data_ingest/v1/experiment/ambyte/v1.0/{CLIENT}/status"

    def setUp(self):
        self.fc = _load()

    def test_payloads(self):
        b = self.fc.build_message
        self.assertEqual(b(_args(cmd="vin-limit", vin_action="get", mv=None), "r"),
                         {"type": "get_vin_limit", "id": "r"})
        for mv in (3700, 4000, 4300, 5000, 15200):
            self.assertEqual(b(_args(cmd="vin-limit", vin_action="set", mv=mv), "r"),
                             {"type": "set_vin_limit", "id": "r", "mv": mv})

    def test_bad_input_refused(self):
        for kw in (dict(vin_action="set", mv=None), dict(vin_action="set", mv=4050),
                   dict(vin_action="set", mv=3600), dict(vin_action="set", mv=15300),
                   dict(vin_action="set", mv=True), dict(vin_action="set", mv="4000"),
                   dict(vin_action="set", mv=4000.0), dict(vin_action="get", mv=4000),
                   dict(vin_action="reset", mv=None)):
            with self.subTest(**kw), self.assertRaises(SystemExit):
                self.fc.build_message(_args(cmd="vin-limit", **kw), "r")

    def test_set_refuses_discovery_and_bad_input_before_aws(self):
        from unittest.mock import patch
        cases = (["vin-limit", "--vin-action", "set", "--mv", "4000"],
                 ["vin-limit", "--vin-action", "set", "--devices", self.CLIENT],
                 ["vin-limit", "--vin-action", "set", "--mv", "4050", "--devices", self.CLIENT],
                 ["vin-limit", "--mv", "4000", "--devices", self.CLIENT])
        for argv in cases:
            with self.subTest(argv=argv), \
                 patch.object(self.fc.fleet_deploy, "boto_session", side_effect=AssertionError("must not touch AWS")), \
                 self.assertRaises(SystemExit) as error:
                self.fc.main(argv)
            self.assertEqual(error.exception.code, 2)

    def _run(self, argv, reply_for):
        import io
        import json
        from contextlib import redirect_stdout
        from types import SimpleNamespace
        from unittest.mock import patch
        future = SimpleNamespace(result=lambda **kw: None)
        sent = []

        def connection(session, subscription, receive, **kwargs):
            def publish(*, topic, payload, qos):
                request = json.loads(payload)
                sent.append(request)
                for reply in reply_for(request):
                    receive(self.STATUS, json.dumps(dict(reply, id=request["id"])).encode())
                return future, 1
            return SimpleNamespace(publish=publish, disconnect=lambda: future)
        aws = SimpleNamespace(mqtt=SimpleNamespace(QoS=SimpleNamespace(AT_LEAST_ONCE=1)))
        out = io.StringIO()
        with patch.dict(sys.modules, awscrt=aws), \
             patch.object(self.fc.fleet_deploy, "boto_session", return_value=object()), \
             patch.object(self.fc.fleet_deploy, "mqtt_connection", connection), \
             patch.object(self.fc.time, "sleep", lambda _: None), redirect_stdout(out):
            rc = self.fc.main([*argv, "--devices", self.CLIENT, "--wait", "1"])
        return rc, sent, out.getvalue()

    def test_get_is_read_only_and_set_is_mutating_and_verified(self):
        got = dict(type="vin_limit_result", ok=True, mv=4300, reg01=6, detail="VIN limit=4300 mV REG01=0x06")
        rc, sent, text = self._run(["vin-limit"], lambda req: [got])
        self.assertEqual(rc, 0, text)
        self.assertEqual(sent[0], {"type": "get_vin_limit", "id": sent[0]["id"]})
        self.assertLessEqual(len(sent[0]["id"].encode()), 64)     # firmware id limit
        self.assertNotIn("MUTATING", text)
        self.assertIn("mv=4300 reg01=0x06", text)

        done = dict(type="set_vin_limit_result", ok=True, mv=4000, reg01=3,
                    detail="VIN limit=4000 mV REG01=0x03 (verified)")
        busy = dict(type="set_vin_limit_result", ok=False, detail="busy")
        rc, sent, text = self._run(["vin-limit", "--vin-action", "set", "--mv", "4000"],
                                   lambda req: [busy, done])      # a QoS1 duplicate first
        self.assertEqual(rc, 0, text)
        self.assertEqual(sent[0], {"type": "set_vin_limit", "id": sent[0]["id"], "mv": 4000})
        self.assertIn("(MUTATING)", text)

    def test_refusal_wrong_readback_or_wrong_reply_type_fails_the_run(self):
        set_argv = ["vin-limit", "--vin-action", "set", "--mv", "4000"]
        refused = dict(type="set_vin_limit_result", ok=False, detail="ESP_ERR_INVALID_RESPONSE")
        rc, _, text = self._run(set_argv, lambda req: [refused])
        self.assertEqual(rc, 1)
        self.assertIn("not confirmed", text)
        other = dict(type="set_vin_limit_result", ok=True, mv=4300, reg01=6, detail="x")
        self.assertEqual(self._run(set_argv, lambda req: [other])[0], 1)          # reads another value
        mismatch = dict(type="set_vin_limit_result", ok=True, mv=4000, reg01=6, detail="x")
        self.assertEqual(self._run(set_argv, lambda req: [mismatch])[0], 1)       # reg01 != mv
        reset_bit = dict(type="set_vin_limit_result", ok=True, mv=4000, reg01=0x83, detail="x")
        self.assertEqual(self._run(set_argv, lambda req: [reset_bit])[0], 1)      # REGISTER RESET set
        no_readback = dict(type="set_vin_limit_result", ok=True, detail="x")
        self.assertEqual(self._run(set_argv, lambda req: [no_readback])[0], 1)
        # A get reply never answers a set (and the other way round): the run times out.
        wrong_type = dict(type="vin_limit_result", ok=True, mv=4000, reg01=3, detail="x")
        rc, _, text = self._run(set_argv, lambda req: [wrong_type])
        self.assertEqual(rc, 1)
        self.assertIn("replies 0/1", text)

    def test_workflow_offers_vin_limit_and_passes_action_and_mv(self):
        import os
        import pathlib
        import subprocess
        import tempfile
        import yaml
        root = pathlib.Path(__file__).resolve().parents[1]
        wf = yaml.safe_load((root / ".github/workflows/fleet-command.yml").read_text())
        inputs = wf[True]["workflow_dispatch"]["inputs"]
        self.assertIn("vin-limit", inputs["command"]["options"])
        self.assertEqual(inputs["vin_limit_action"]["options"], ["get", "set"])
        self.assertEqual(inputs["vin_limit_action"]["default"], "get")
        self.assertLessEqual(len(inputs), 25)    # GitHub's workflow_dispatch input limit
        step = next(s for s in wf["jobs"]["command"]["steps"] if s.get("id") == "run")
        self.assertEqual(step["env"]["VIN_ACTION"], "${{ inputs.vin_limit_action }}")
        self.assertEqual(step["env"]["VIN_MV"], "${{ inputs.vin_limit_mv }}")
        script = step["run"].replace("${{ inputs.environment }}", "dev")

        def argv_for(action, mv, devices):
            with tempfile.TemporaryDirectory() as tmp:
                fake = pathlib.Path(tmp) / "python"
                fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$ARGV_OUT"\n')
                fake.chmod(0o755)
                env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}", ARGV_OUT=f"{tmp}/argv",
                           GITHUB_STEP_SUMMARY=f"{tmp}/summary", COMMAND="vin-limit", DEVICES=devices,
                           TIMEZONE="", EXPECT_OFFSET="", WINDOW="1440", EXPECT_FW="", MODE="status", TOKEN="",
                           FROM_ID="0", TO_ID="0", FROM_MS="0", TO_MS="0", CHUNK="64", WAIT="90",
                           DRY_RUN="false", LOG_NAME="", LOG_TAIL="65536", PP_ACTION="status", PP_STATE="no_flash",
                           VIN_ACTION=action, VIN_MV=mv)
                r = subprocess.run(["bash", "-c", script], cwd=tmp, env=env, capture_output=True, text=True)
                argv = pathlib.Path(f"{tmp}/argv").read_text().splitlines() if r.returncode == 0 else None
                return r.returncode, argv

        rc, argv = argv_for("set", "4000", self.CLIENT)
        self.assertEqual(rc, 0)
        self.assertEqual(argv[1], "vin-limit")
        self.assertEqual(argv[argv.index("--vin-action") + 1], "set")
        self.assertEqual(argv[argv.index("--mv") + 1], "4000")
        rc, argv = argv_for("get", "4000", "")
        self.assertEqual(rc, 0)
        self.assertNotIn("--mv", argv)          # the value only rides with set
        self.assertEqual(argv_for("set", "4000", " ")[0], 2)      # mutating without explicit devices
        self.assertEqual(argv_for("set", "", self.CLIENT)[0], 2)  # set without a value
        self.assertEqual(argv_for("set", "4.0", self.CLIENT)[0], 2)


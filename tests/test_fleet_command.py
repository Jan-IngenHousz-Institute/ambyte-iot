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
    def chunk(name, data, offset, size, eof, **extra):
        import base64
        reply = {"type": "log_read_result", "ok": True, "detail": "ok", "name": name, "offset": offset,
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
            (self.chunk("ambyte.log", b"x", 7, 9, True), "offset 7"),                # wrong offset
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
        self.assertEqual(st.feed(self.chunk("ambyte.log", b"", 4, 3, True)), "error")   # rotated under us
        self.assertIn("shrank", st.error)

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
        listing = {"type": "log_list_result", "ok": True, "detail": "ok", "dir": "/sdcard/logs",
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

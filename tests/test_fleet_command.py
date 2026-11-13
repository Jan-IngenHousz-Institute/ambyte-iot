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

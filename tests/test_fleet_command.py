"""fleet_command.py builds the right device payload and refuses unsafe replays."""
import argparse
import importlib
import pathlib
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

    def test_devices_keep_case(self):
        devs = self.fc.parse_devices("ambyte_28:37:2F:FF:FC:80, 28:37:2F:FF:E7:50")
        self.assertIn("ambyte_28:37:2F:FF:FC:80", devs)
        self.assertIn("AMBYTE_28:37:2F:FF:E7:50", devs)


if __name__ == "__main__":
    unittest.main()

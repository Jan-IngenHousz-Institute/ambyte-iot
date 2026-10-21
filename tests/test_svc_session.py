# SPDX-License-Identifier: GPL-3.0-or-later
"""Real bounded CLI mode loop with production dispatch/storage, split USB IO."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

from test_svc_transactions import begin, frame, put, STORE

ROOT = Path(__file__).resolve().parents[1]


class ServiceSessionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "session"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-DSVC_HOST",
            "-I" + str(ROOT / "components/usb_service/include"),
            *(str(ROOT / "components/usb_service" / name) for name in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c", "svc_session.c")),
            str(ROOT / "tests/svc_host/session_driver.c"), "-Wl,--wrap=stat", "-lcrypto", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "svc"

    def call(self, data, size=64, env=None, raw=False):
        p = subprocess.run([str(self.binary), str(self.root), str(size)], input=data, capture_output=True,
                           env={**os.environ, **(env or {})})
        self.assertEqual(p.returncode, 0, p.stderr.decode())
        if raw: return p.stdout
        responses = []
        for line in p.stdout.splitlines(keepends=True):
            prefix, crc = line[:-1].rsplit(b"*", 1)
            self.assertEqual(crc, b"%08x" % zlib.crc32(prefix))
            self.assertEqual(len(prefix[:8]), 8)
            self.assertLessEqual(len(line), 1043)
            responses.append((int(prefix[:8], 16), json.loads(prefix[9:])))
        self.assertEqual(responses[0], (0, {"ok": True, "store": STORE, "service": 1, "state": "ready"}))
        return responses

    def test_entry_split_requests_and_exit_have_only_frame_bytes(self):
        data = frame(f"svc.bind {STORE}", 1) + frame(begin(), 2) + frame(put(1, 0, b"abc"), 3) + frame("svc.exit", 4)
        responses = self.call(data, size=1)
        self.assertEqual([id for id, response in responses], [0, 1, 2, 3, 4])
        self.assertEqual(responses[-1][1]["state"], "exit")
        self.assertEqual((self.root / "stage.bin").read_bytes(), b"abc")
        self.assertEqual(self.call(frame(f"svc.bind {STORE}") + frame("svc.result 0000000000000001", 2) + frame("svc.exit", 3))[2][1]["offset"], 3)

    def test_sustained_ready_service_traffic_has_blocking_opportunities(self):
        # Production session/engine/store, immediate RX/TX, synchronous flash
        # CPU cost. Neither a full read nor a successful write implies a task
        # blocked: positive idle time must occur during the sustained burst.
        data=b"".join(frame("svc.version" if i%2 else "svc.stats",i) for i in range(1,1001))
        data+=frame("svc.exit",1001)
        responses=self.call(data,env={"SVC_WDT_MODEL":"1"})
        self.assertEqual(len(responses),1002)
        self.assertTrue(all(reply["ok"] for _,reply in responses))

    def test_invalid_and_oversized_suffixes_never_execute(self):
        data = b"\0" + frame(begin()) + b"x" * 300 + frame(begin())
        data += frame("svc.version", 9) + frame("svc.exit", 10)
        responses = self.call(data)
        self.assertEqual([r.get("reason") for _, r in responses if not r["ok"]], ["bad_frame", "line_too_long"])
        version = next(r for id, r in responses if id == 9)
        self.assertEqual(version["highest_id"], "0000000000000000")

    def test_partial_timeout_answers_the_first_complete_probe(self):
        data = b"00000007 svc.vers\x1e" + frame("svc.version", 20) + frame("svc.exit", 21)
        responses = self.call(data)
        self.assertEqual([cid for cid, _ in responses], [0, 0, 20, 21])
        self.assertEqual(responses[1][1]["reason"], "frame_timeout")
        self.assertEqual(responses[2][1]["highest_id"], "0000000000000000")

    def test_timeout_discards_old_prefix_and_rejects_delayed_tail(self):
        request = frame(begin(), 8)
        data = request[:25] + b"\x1e" + request[25:] + frame("svc.version", 20) + frame("svc.exit", 21)
        responses = self.call(data)
        self.assertEqual(responses[1][1]["reason"], "frame_timeout")
        self.assertEqual(len(responses), 5)
        self.assertFalse(responses[2][1]["ok"])
        self.assertIn(responses[2][1]["reason"], ("bad_frame", "bad_crc"))
        self.assertEqual(responses[3][0], 20)
        self.assertEqual(responses[3][1]["highest_id"], "0000000000000000")

    def test_idle_exit_does_not_abort_durable_staging(self):
        responses = self.call(frame(f"svc.bind {STORE}") + frame(begin(), 2))
        self.assertEqual(responses[-1][1]["reason"], "session_timeout")
        responses = self.call(frame(f"svc.bind {STORE}") + frame("svc.result 0000000000000001", 2) + frame("svc.exit", 3))
        self.assertEqual(responses[2][1]["state"], "staging")

    def test_overrun_discards_burst_and_invalidates_binding(self):
        data = frame(f"svc.bind {STORE}") + b"\x1f" + frame(begin(), 2) * 20 + b"\x1e"
        data += frame(begin(), 3) + frame(f"svc.bind {STORE}", 4) + frame("svc.version", 5) + frame("svc.exit", 6)
        responses = self.call(data)
        self.assertEqual([r["reason"] for _, r in responses if not r["ok"]], ["rx_overrun", "not_bound"])
        self.assertEqual(next(r for id, r in responses if id == 5)["highest_id"], "0000000000000000")

    def test_entry_forgets_a_previous_sessions_failed_bind(self):
        data = frame("svc.result 0000000000000001", 1) + frame("svc.exit", 2)
        responses = self.call(data, env={"SVC_STALE_SESSION": "1"})
        self.assertEqual(responses[1][1]["reason"], "not_bound")

    def test_overrun_retains_a_wrong_instance_only_until_session_exit(self):
        data = frame("svc.bind 2222222222222222", 1)
        data += b"\x1f" + frame("svc.version", 2) + b"\x1e"
        data += frame("svc.result 0000000000000001", 3) + frame("svc.exit", 4)
        responses = self.call(data)
        self.assertEqual([r["reason"] for _, r in responses if not r["ok"]],
                         ["store_changed", "rx_overrun", "store_changed"])

    def test_every_exit_path_forgets_the_attempted_instance(self):
        wrong_bind = frame("svc.bind 2222222222222222", 1)
        with self.subTest(exit="idle timeout"):
            responses = self.call(wrong_bind)
            self.assertEqual(responses[-1][1]["reason"], "session_timeout")
        with self.subTest(exit="transport failure"):
            self.call(wrong_bind, env={"SVC_TX_FAIL": "2"}, raw=True)
        with self.subTest(exit="entry failure"):
            self.assertEqual(self.call(b"", env={"SVC_ENTER_FAIL": "1", "SVC_STALE_SESSION": "1"}, raw=True), b"")

    def test_fresh_active_discovery_and_overrun_rebind_preserve_durable_prefix(self):
        self.call(frame(f"svc.bind {STORE}") + frame(begin(), 2) + frame(put(1, 0, b"abc"), 3) + frame("svc.exit", 4))
        data = frame("svc.active", 10) + frame("svc.result 0000000000000001", 11)
        data += frame(f"svc.bind {STORE}", 12) + b"\x1f" + frame(begin(op=2), 13) + b"\x1e"
        data += frame("svc.result 0000000000000001", 14) + frame(f"svc.bind {STORE}", 15)
        data += frame("svc.result 0000000000000001", 16) + frame("svc.exit", 17)
        values = dict(self.call(data))
        self.assertEqual(values[10]["active"], dict(op="0000000000000001", kind="schedule", state="staging"))
        self.assertEqual(values[11]["reason"], "not_bound")
        self.assertEqual(values[14]["reason"], "not_bound")
        self.assertEqual(values[16]["offset"], 3)
        self.assertEqual((self.root / "stage.bin").read_bytes(), b"abc")

    def test_active_requests_use_the_ordinary_idle_timeout(self):
        values = self.call(frame("svc.active", 8))
        self.assertIsNone(values[1][1]["active"])
        self.assertEqual(values[-1][1]["reason"], "session_timeout")

    def test_tx_failure_ends_mode_without_losing_the_admitted_operation(self):
        data = frame(f"svc.bind {STORE}") + frame(begin(), 2)
        output = self.call(data, env={"SVC_TX_FAIL": "3"}, raw=True)
        self.assertTrue(output.endswith(b"00000"))
        responses = self.call(frame(f"svc.bind {STORE}") + frame("svc.result 0000000000000001", 2) + frame("svc.exit", 3))
        self.assertEqual(responses[2][1]["state"], "staging")
        self.assertEqual(self.call(b"", env={"SVC_ENTER_FAIL": "1"}, raw=True), b"")


if __name__ == "__main__":
    unittest.main()

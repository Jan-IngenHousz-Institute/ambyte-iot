# SPDX-License-Identifier: GPL-3.0-or-later
"""Production protocol + CRC codec + durable files, under ASan/UBSan."""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
STORE = "0123456789abcdef"
SHA = hashlib.sha256(b"abc").hexdigest()


def frame(body, cid=1):
    line = f"{cid:08x} {body}".encode()
    return line + b"*%08x\n" % zlib.crc32(line)


def begin(op=1, size=3, sha=SHA, version="a3", fw="test", workbook="-"):
    return f"schedule.begin {op:016x} {size:08x} {sha} {version} {fw} {workbook}"


def put(op, offset, data, crc=None):
    return (f"p {op:016x} {offset:08x} {zlib.crc32(data) if crc is None else crc:08x} "
            + base64.b64encode(data).decode())


def join(op=1, ssid=b"A3Bench", password=b"wrongpass"):
    return (f"svc.wifi.join {op:016x} {base64.b64encode(ssid).decode()} "
            + (base64.b64encode(password).decode() if password else "-"))


class ServiceTransactionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "protocol"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-DSVC_HOST",
            "-I" + str(ROOT / "components/usb_service/include"),
            *(str(ROOT / "components/usb_service" / name) for name in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c")),
            str(ROOT / "tests/svc_host/protocol_driver.c"), "-Wl,--wrap=svc_store_get",
            "-Wl,--wrap=svc_store_admit", "-lcrypto", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "svc"
        self.addCleanup(self.temp.cleanup)

    def call(self, *bodies, bind=True, env=None, raw=False):
        requests = ([f"svc.bind {STORE}"] if bind else []) + list(bodies)
        data = b"".join(item if isinstance(item, bytes) else
                        item.encode() + b"\n" if item.startswith("@") else frame(item, i + 1)
                        for i, item in enumerate(requests))
        result = subprocess.run([str(self.binary), str(self.root)], input=data, capture_output=True,
                                env={**os.environ, **(env or {})})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        lines = result.stdout.splitlines(keepends=True)
        parsed = []
        for line in lines:
            self.assertLessEqual(len(line), 1043)
            prefix, crc = line[:-1].rsplit(b"*", 1)
            self.assertEqual(crc, b"%08x" % zlib.crc32(prefix))
            self.assertRegex(prefix[:8], b"^[0-9a-f]{8}$")
            parsed.append(json.loads(prefix[9:]))
        if bind:
            self.assertEqual(parsed.pop(0), {"ok": True, "store": STORE, "state": "bound"})
            lines.pop(0)
        return lines if raw else parsed

    def reason(self, response, reason):
        self.assertEqual(response, {"ok": False, "store": STORE, "reason": reason})

    def test_golden_wire_and_discovery(self):
        lines = self.call("svc.version", bind=False, raw=True)
        self.assertEqual(lines, [b'00000001 {"ok":true,"store":"0123456789abcdef","service":1,"actions":["svc.version","svc.bind","svc.result","svc.stats","schedule.begin","p","schedule.commit","svc.abort","svc.wifi.join","svc.wifi.status","svc.exit","svc.active"],"max_line":248,"chunk":144,"result_slots":8,"highest_id":"0000000000000000"}*7215c2b2\n'])
        self.call(begin())
        lines = self.call(frame(put(1, 0, b"abc"), 5), frame("svc.abort 0000000000000001", 6),
                          frame("svc.result 0000000000000001", 9), raw=True)
        self.assertEqual(lines[0], b'00000005 {"ok":true,"store":"0123456789abcdef","op":"0000000000000001","state":"staging","offset":3}*7ed9e192\n')
        self.assertEqual(lines[1], b'00000006 {"ok":true,"store":"0123456789abcdef","op":"0000000000000001","state":"aborted","reason":"aborted"}*c835d4f4\n')
        self.assertTrue(lines[2].endswith(b'*69341aa2\n'))

    def test_binding_is_scoped_and_wrong_instance_clears_it(self):
        responses = self.call(begin(), f"svc.bind {STORE}", begin(), "@unbind", put(1, 0, b"abc"),
                              "svc.bind 1111111111111111", "svc.result 0000000000000001", bind=False)
        self.reason(responses[0], "not_bound")
        self.assertEqual(responses[2]["state"], "staging")
        self.reason(responses[3], "not_bound")
        for response in responses[4:]: self.reason(response, "store_changed")
        self.assertEqual(self.call("svc.version")[0]["highest_id"], "0000000000000001")

    def test_descriptor_replay_conflict_and_busy(self):
        responses = self.call(begin(), put(1, 0, b"abc"), begin(), begin(version="other"),
                              begin(size=4), begin(sha="a" * 64), begin(fw="x"),
                              begin(workbook="01234567-89ab-cdef-0123-456789abcdef"), join(), begin(op=2),
                              "svc.result 0000000000000001")
        self.assertEqual(responses[2]["offset"], 3)
        for response in responses[3:9]: self.reason(response, "request_conflict")
        self.reason(responses[9], "busy")
        self.assertEqual(responses[10]["sha256"], SHA)
        self.assertIsNone(responses[10]["workbook"])

    def test_active_is_unbound_read_only_and_survives_restart(self):
        self.assertIsNone(self.call("svc.active", bind=False)[0]["active"])
        self.call(begin())
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        for value in self.call(*(["svc.active"] * 20), bind=False):
            self.assertEqual(value, dict(ok=True, store=STORE, active=dict(op="0000000000000001", kind="schedule", state="staging")))
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.root.iterdir()})
        self.call("svc.abort 0000000000000001")
        self.assertIsNone(self.call("svc.active", bind=False)[0]["active"])
        value = self.call(join(op=2), "svc.active")[1]
        self.assertEqual(value["active"], dict(op="0000000000000002", kind="wifi", state="connecting"))
        self.reason(self.call("svc.active extra", bind=False)[0], "invalid_args")

    def test_active_does_not_guess_from_the_highest_watermark(self):
        values = self.call(begin(), "@highest 0000000000000005", "svc.version", "svc.active")
        self.assertEqual(values[1]["highest_id"], "0000000000000005")
        self.assertEqual(values[2]["active"]["op"], "0000000000000001")

    def test_known_store_mismatch_survives_unbind_but_not_fresh_session(self):
        other = "1111111111111111"
        values = self.call("@instance " + other, "@unbind", begin(), "svc.bind " + other, begin(), "svc.active")
        self.assertEqual(values[0], dict(ok=False, store=other, reason="store_changed"))
        self.assertEqual(values[1]["state"], "bound")
        self.assertEqual(values[2]["state"], "staging")
        self.assertEqual(values[3]["active"]["op"], "0000000000000001")

    def test_unbound_mutations_have_no_admission_or_worker_side_effect(self):
        bodies = [begin(), put(1, 0, b"abc"), "schedule.commit 0000000000000001",
                  "svc.abort 0000000000000001", join(), "svc.result 0000000000000001"]
        for value in self.call(*bodies, bind=False, env={"SVC_DENY_ACCESS": "1"}):
            self.reason(value, "not_bound")
        self.assertEqual(self.call("svc.version")[0]["highest_id"], "0000000000000000")
        self.assertFalse((self.root / "stage.bin").exists())

    def test_active_does_not_hide_corrupt_store_as_empty(self):
        self.call(begin())
        (self.root / "store").write_bytes(b"corrupt")
        self.assertEqual(self.call("svc.active", bind=False)[0], dict(ok=False, store=None, reason="corrupt_store"))

    def test_strict_grammar_does_not_consume_ids(self):
        bodies = ["", " svc.version", "svc.version ", "svc.version  extra",
                  "svc.version extra", "svc.bind", "svc.bind 0", "svc.result 0000000000000000",
                  "svc.result 000000000000000A", begin().replace("00000003", "0000003"),
                  begin(sha="A" * 64), begin(version="a/b"), begin(workbook="bad"),
                  begin() + " extra", "svc.abort +000000000000001"]
        for response in self.call(*bodies): self.reason(response, "invalid_args")
        self.reason(self.call("svc.version\t")[0], "bad_frame")
        self.reason(self.call("unsupported.action")[0], "unsupported")
        self.assertEqual(self.call("svc.version")[0]["highest_id"], "0000000000000000")

    def test_all_chunk_errors_and_maximum_wire_path(self):
        self.call(begin(size=288))
        responses = self.call(put(1, 0, b"a" * 144, crc=0), put(1, 144, b"a" * 144),
                              put(1, 0, b"abc"), "@dispatch " + put(1, 0, b"a" * 145),
                              "p 0000000000000001 00000000 352441c2 YWJ=", put(1, 0, b"a" * 144),
                              put(1, 0, b"b" * 144), put(1, 1, b"a" * 144),
                              put(1, 144, b"b" * 144), put(1, 0, b"a" * 144))
        for response, reason in zip(responses[:5], ("chunk_crc", "offset_gap", "offset_conflict", "size_limit", "invalid_args")):
            self.reason(response, reason)
        self.reason(responses[6], "offset_conflict"); self.reason(responses[7], "offset_conflict")
        self.assertEqual(responses[-1]["offset"], 288)
        self.assertEqual(len(frame(put(1, 0, b"a" * 144))), 248)

    def test_commit_is_asynchronous_and_terminal_table(self):
        responses = self.call(begin(), "schedule.commit 0000000000000001", put(1, 0, b"abc"),
                              "schedule.commit 0000000000000001", "schedule.commit 0000000000000001",
                              put(1, 0, b"abc"), "svc.abort 0000000000000001", "svc.stats", "@finish",
                              "schedule.commit 0000000000000001", put(1, 0, b"abc"), "svc.abort 0000000000000001")
        self.reason(responses[1], "incomplete")
        self.assertEqual(responses[3]["state"], "committing")
        self.assertEqual(responses[4]["state"], "committing")
        for response in responses[5:7]: self.reason(response, "commit_started")
        self.assertEqual(responses[7]["worker_stack"], 1)  # one submit, no replay
        self.assertEqual(responses[8]["state"], "succeeded")
        for response in responses[9:]: self.reason(response, "operation_complete")
        responses = self.call(begin(op=2), "svc.abort 0000000000000002", "svc.abort 0000000000000002",
                              "schedule.commit 0000000000000002", put(2, 0, b"abc"))
        self.assertEqual(responses[1], responses[2])
        for response in responses[3:]: self.reason(response, "operation_complete")

    def test_worker_rejection_is_durable_and_not_requeued(self):
        responses = self.call(begin(), put(1, 0, b"abc"), "schedule.commit 0000000000000001", env={"SVC_REJECT": "1"})
        self.assertEqual((responses[-1]["state"], responses[-1]["reason"]), ("failed", "busy"))
        self.assertEqual(self.call("schedule.commit 0000000000000001")[0], responses[-1])

    def test_wifi_descriptor_reuse_secret_wipe_and_restore_schema(self):
        responses = self.call(join(), join(), join(password=b"different"), "schedule.commit 0000000000000001",
                              "svc.abort 0000000000000001", "svc.stats", "@finish", join(), "svc.result 0000000000000001")
        self.assertEqual(responses[0], responses[1]); self.reason(responses[2], "request_conflict")
        for response in responses[3:5]: self.reason(response, "invalid_sequence")
        self.assertEqual(responses[5]["worker_stack"], 1)
        self.assertEqual(responses[6], responses[7])
        self.assertTrue(responses[-1]["saved"]); self.assertTrue(responses[-1]["connected"])
        self.assertEqual(responses[-1]["ssid"], "A3Bench")
        content = b"".join(path.read_bytes() for path in self.root.iterdir()) + json.dumps(responses).encode()
        for secret in (b"wrongpass", base64.b64encode(b"wrongpass"), b"different", b"fingerprint"):
            self.assertNotIn(secret, content)

    def test_wifi_validation_and_status_separates_trial(self):
        requests = [join(ssid=b"a" * 32), join(ssid=b"\0"), join(ssid=b"\xc0\x80"),
                    join(password=b"a" * 64), join(password=b"\n" * 8), join(password=b"short")]
        responses = self.call(*requests)
        for response in responses[:-1]: self.reason(response, "invalid_args")
        self.reason(responses[-1], "wifi_invalid_credentials")
        self.assertEqual(self.call("svc.version")[0]["highest_id"], "0000000000000000")
        status = self.call("svc.wifi.status", env={"SVC_TRIAL": "1"})[0]
        self.assertEqual((status["configured_ssid"], status["trial_ssid"], status["trial"]), ("original", "trial", True))
        self.assertIsNone(self.call("svc.wifi.status")[0]["trial_ssid"])
        self.reason(self.call("svc.wifi.status", env={"SVC_NO_WIFI": "1"})[0], "wifi_unavailable")
        responses = self.call(join(ssid="网络🌱".encode(), password=b""))
        self.assertEqual(responses[0]["ssid"], "网络🌱")

    def test_space_and_hash_errors_have_no_admission(self):
        self.reason(self.call(begin(size=16385))[0], "size_limit")
        self.reason(self.call(begin(), env={"SVC_NO_SPACE": "1"})[0], "no_space")
        self.reason(self.call(join(), env={"SVC_HASH_FAIL": "1"})[0], "storage_error")
        self.assertEqual(self.call("svc.version")[0]["highest_id"], "0000000000000000")


if __name__ == "__main__":
    unittest.main()

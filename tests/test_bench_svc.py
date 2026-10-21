"""Bench diagnostics over real production framing/dispatch/session code."""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

from flash_gui.service_benchmark import ServiceBenchmarkClient, measure_stage, projections
from flash_gui.service_client import OperationJournal, ServiceClient, ServiceClientError
from test_svc_python_client import PipeSerial
from test_svc_transactions import frame, begin, put, STORE
from tools.bench_svc import padded_yaml

ROOT = Path(__file__).resolve().parents[1]


def data(offset, count):
    return bytes((i * 73 + 19) % 256 for i in range(offset, offset + count))


def compile_peer(binary, *, bench, session):
    names = ["svc_wire.c", "svc_store.c", "svc_protocol.c"]
    if session: names += ["svc_engine.c", "svc_session.c"]
    if bench: names += ["svc_bench.c"]
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DSVC_HOST",
        *(["-DCONFIG_AMBYTE_SVC_BENCH=1"] if bench else []),
        "-I" + str(ROOT / "components/usb_service/include"),
        *(str(ROOT / "components/usb_service" / name) for name in names),
        str(ROOT / "tests/svc_host" / ("client_peer.c" if session else "protocol_driver.c")),
        *([] if session else ["-Wl,--wrap=svc_store_get", "-Wl,--wrap=svc_store_admit"]),
        "-lcrypto", "-o", str(binary)], check=True)


class BenchmarkProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "bench"
        cls.release = Path(cls.build.name) / "release"
        compile_peer(cls.binary, bench=True, session=False)
        compile_peer(cls.release, bench=False, session=False)

    @classmethod
    def tearDownClass(cls): cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "svc"

    def call(self, *bodies, bind=True, release=False, raw=False):
        bodies = ([f"svc.bind {STORE}"] if bind else []) + list(bodies)
        encoded = b"".join((body + "\n").encode() if body.startswith("@") else frame(body, i + 1)
                           for i, body in enumerate(bodies))
        result = subprocess.run([str(self.release if release else self.binary), str(self.root)],
                                input=encoded, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        lines = result.stdout.splitlines(keepends=True)
        parsed = []
        for line in lines:
            self.assertLessEqual(len(line), 1043)
            prefix, crc = line[:-1].rsplit(b"*", 1)
            self.assertEqual(int(crc, 16), zlib.crc32(prefix))
            parsed.append(json.loads(prefix[9:]))
        return (lines[1:] if bind else lines) if raw else (parsed[1:] if bind else parsed)

    def test_release_neither_advertises_nor_executes_diagnostic_actions(self):
        values = self.call("svc.version", "bench.begin rx 00100000 00000090", "bench.read 00000000 00000090",
                           "bench.end", "bench.abort", put(0, 0, data(0, 144)), release=True)
        self.assertFalse(any(a.startswith("bench.") for a in values[0]["actions"]))
        self.assertEqual([v["reason"] for v in values[1:]], ["unsupported"] * 4 + ["invalid_args"])
        bench = self.call("svc.version")[0]
        self.assertTrue({"bench.begin", "bench.read", "bench.end", "bench.abort"} <= set(bench["actions"]))

    def test_grammar_limits_binding_and_mode_ownership(self):
        self.assertEqual(self.call("bench.begin rx 00100000 00000090", bind=False)[0]["reason"], "not_bound")
        values = self.call("bench.begin rx 00000000 00000090", "bench.begin rx 00100001 00000090",
            "bench.begin rx 00100000 000002be", "bench.begin tx 00100000 000002bf",
            "bench.begin bad 00100000 00000090", "bench.begin rx 00100000 90",
            "bench.end", "bench.read 00000000 00000090", "bench.unknown",
            "bench.begin rx 00000090 00000090", "bench.begin rx 00000090 00000090",
            begin(), "schedule.commit 0000000000000001", "svc.abort 0000000000000001",
            "bench.end", "bench.abort", "bench.abort")
        self.assertEqual([v.get("reason") for v in values], ["invalid_args", "size_limit", "size_limit", "size_limit",
            "invalid_args", "invalid_args", "invalid_sequence", "invalid_sequence", "unsupported",
            None, "busy", "busy", "busy", "busy", "incomplete", None, None])

    def test_ingress_crc_offsets_replay_exact_ack_and_no_persistent_change(self):
        self.call("svc.version")
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        values = self.call("bench.begin rx 00000121 00000090", put(0, 0, data(0, 144), crc=0),
            put(0, 0, b"x" * 144), put(0, 144, data(144, 144)), put(0, 1, data(1, 144)),
            put(0, 0, data(0, 143)), put(0, 0, data(0, 144)), put(0, 0, data(0, 144)),
            put(0, 144, data(144, 144)), put(0, 288, data(288, 1)), "bench.end", "svc.version")
        self.assertEqual([v["reason"] for v in values[1:6]],
                         ["chunk_crc", "checksum_mismatch", "offset_gap", "offset_gap", "offset_conflict"])
        self.assertEqual([v["offset"] for v in values[6:10]], [144, 144, 288, 289])
        self.assertEqual(values[6], dict(ok=True, store=STORE, op="0000000000000000", state="staging", offset=144))
        self.assertEqual(values[10]["crc32"], f"{zlib.crc32(data(0, 289)):08x}")
        self.assertEqual(values[-1]["highest_id"], "0000000000000000")
        self.assertEqual({p.name: p.read_bytes() for p in self.root.iterdir()}, before)
        self.assertEqual(len(frame(put(0, 0, data(0, 144)))), 248)

    def test_egress_both_sizes_largest_frame_checksums_and_replays(self):
        for chunk in (144, 702):
            with self.subTest(chunk=chunk):
                values = self.call(f"bench.begin tx {chunk + 1:08x} {chunk:08x}",
                    f"bench.read 00000000 {chunk:08x}", f"bench.read 00000000 {chunk:08x}",
                    f"bench.read {chunk:08x} 00000001", "bench.end")
                self.assertEqual(values[1], values[2])
                for reply, offset, count in ((values[1], 0, chunk), (values[3], chunk, 1)):
                    decoded = base64.b64decode(reply["data"], validate=True)
                    self.assertEqual(decoded, data(offset, count))
                    self.assertEqual(reply["crc32"], f"{zlib.crc32(decoded):08x}")
                self.assertEqual(values[-1]["crc32"], f"{zlib.crc32(data(0, chunk + 1)):08x}")
        lines = self.call("bench.begin tx 000002be 000002be", "bench.read 00000000 000002be", raw=True)
        self.assertEqual(len(lines[-1]), 1042)
        self.assertEqual(len(lines[-1][9:-10]), 1023)
        self.assertEqual(self.call("bench.begin tx 000002bf 000002be", "bench.read 00000000 000002bf")[-1]["reason"], "size_limit")

    def test_unbind_restart_and_real_active_operation_cannot_be_taken_over(self):
        self.call("bench.begin rx 00000090 00000090", put(0, 0, data(0, 144)))
        self.assertEqual(self.call("bench.end")[0]["reason"], "invalid_sequence")
        values = self.call("bench.begin rx 00000090 00000090", "@unbind", f"svc.bind {STORE}", "bench.end")
        self.assertEqual(values[-1]["reason"], "invalid_sequence")
        values = self.call(begin(), "bench.begin rx 00000090 00000090", "svc.result 0000000000000001")
        self.assertEqual(values[1]["reason"], "busy")
        self.assertEqual(values[2]["state"], "staging")


class FastPipe(PipeSerial):
    @property
    def in_waiting(self):
        import array, fcntl, termios
        pending = array.array("i", [0])
        fcntl.ioctl(self.process.stdout, termios.FIONREAD, pending, True)
        return pending[0]


class AlteredPipe:
    """Alter one real C reply, then reframe it with a valid wire CRC."""
    def __init__(self, port, problem):
        self.port, self.problem = port, problem
        self.pending = bytearray()
        self.timeout, self.write_timeout = .1, 5.0
    @property
    def in_waiting(self): return len(self.pending)
    def write(self, value): return self.port.write(value)
    def read(self, size):
        if not self.pending:
            line = bytearray()
            while not line.endswith(b"\n"):
                chunk = self.port.read(self.port.in_waiting or 1)
                if not chunk: return b""
                line.extend(chunk)
            prefix = line[:-1].rsplit(b"*", 1)[0]
            value = json.loads(prefix[9:])
            if "data" in value:
                if self.problem == "offset": value["offset"] = "ffffffff"
                if self.problem == "chunk_crc": value["crc32"] = "00000000"
                if self.problem == "payload":
                    changed = b"x" * len(base64.b64decode(value["data"]))
                    value.update(data=base64.b64encode(changed).decode(), crc32=f"{zlib.crc32(changed):08x}")
                if self.problem == "base64": value["data"] = "!!!!"
            if value.get("state") == "complete" and self.problem == "final_crc": value["crc32"] = "00000000"
            prefix = prefix[:9] + json.dumps(value, separators=(",", ":")).encode()
            self.pending.extend(prefix + b"*%08x\n" % zlib.crc32(prefix))
        result = bytes(self.pending[:size]); del self.pending[:size]
        return result


class BenchmarkClientIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "bench"
        cls.release = Path(cls.build.name) / "release"
        compile_peer(cls.binary, bench=True, session=True)
        compile_peer(cls.release, bench=False, session=True)

    @classmethod
    def tearDownClass(cls): cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def peer(self, *, release=False, no_writes=False):
        with patch.dict(os.environ, {"SVC_ASSERT_NO_WRITES": "1"} if no_writes else {}):
            port = FastPipe(self.release if release else self.binary, self.root / "device")
        self.addCleanup(port.close)
        return port

    def test_actual_one_mib_both_directions_and_sizes_without_storage_mutation(self):
        port = self.peer(no_writes=True)
        client = ServiceBenchmarkClient(port, None)
        expected = hashlib.sha256(data(0, 1048576)).hexdigest()
        reports = []
        for direction, chunk in (("rx", 144), ("tx", 144), ("tx", 702)):
            report = client.transfer(direction, chunk)
            self.assertEqual(report["payload_bytes"], 1048576)
            self.assertEqual(report["payload_sha256"], expected)
            self.assertEqual(report["crc32"], f"{zlib.crc32(data(0, 1048576)):08x}")
            self.assertGreater(report["effective_bytes_s"], 0)
            self.assertGreaterEqual(report["rtt_p95_s"], report["rtt_median_s"])
            self.assertEqual(report["frames"], (1048576 + chunk - 1) // chunk + 2)
            reports.append(report)
        self.assertEqual(len(list((self.root / "device").iterdir())), 1)  # initialized watermark only
        self.assertIn("apply_count=0", port.close())
        projected = projections(reports)
        self.assertEqual(len(projected["rows"]), 3)
        self.assertIn("remeasure", projected["assumptions"])
        # These are host-pipe correctness tests, not device throughput evidence.

    def test_benchmark_refuses_release_without_sending_diagnostic_action(self):
        port = self.peer(release=True)
        with self.assertRaisesRegex(ServiceClientError, "svc-bench"):
            ServiceBenchmarkClient(port, None).transfer("rx", 144)
        self.assertEqual(port.puts, 0)

    def test_bad_device_payload_offsets_or_checksums_never_produce_a_report(self):
        for problem in ("offset", "chunk_crc", "payload", "base64", "final_crc"):
            with self.subTest(problem=problem):
                port = self.peer(no_writes=True)
                client = ServiceBenchmarkClient(AlteredPipe(port, problem), None)
                with self.assertRaises(ServiceClientError): client.transfer("tx", 144, size=288)
                self.assertIn("apply_count=0", port.close())

    def test_real_store_measurement_uses_shared_put_and_aborts_without_install(self):
        port = self.peer(release=True)
        client = ServiceClient(port, OperationJournal(self.root / "host", "02:00:00:00:00:01"))
        blob = padded_yaml((ROOT / "tests/identity_host/bench.yaml").read_bytes())
        report = measure_stage(client, blob, "a3", "test", "11111111-2222-4333-8444-555555555555")
        self.assertEqual((report["payload_bytes"], report["cleanup"], port.puts, port.commits), (16384, "aborted", 114, 0))
        self.assertFalse((self.root / "device/stage.bin").exists())
        self.assertEqual(client.journal.load()["phase"], "complete")
        self.assertIn("apply_count=0", port.close())

    def test_yaml_padding_preserves_original_and_checks_bounds(self):
        blob = padded_yaml(b"schema: jii.ambyte-schedule/v1-draft\n")
        self.assertEqual(len(blob), 16384)
        self.assertTrue(blob.startswith(b"schema: jii.ambyte-schedule/v1-draft\n#"))
        for value in (b"", b"no LF", b"\0\n", b"x" * 16384 + b"\n"):
            with self.assertRaises(ValueError): padded_yaml(value)

    def test_partial_saved_stage_is_not_misreported_as_full_upload_rate(self):
        journal = OperationJournal(self.root / "host", "02:00:00:00:00:01")
        journal.save(dict(schema=1, mac=journal.mac, store=STORE, op="0000000000000001", phase="admitted",
            descriptor=dict(kind="schedule", size=16384, sha256="a" * 64, version="a3", built_fw="test", workbook=None)))
        port = self.peer(release=True)
        with self.assertRaisesRegex(ServiceClientError, "fresh full upload"):
            measure_stage(ServiceClient(port, journal), b"x" * 16384, "a3", "test")
        self.assertEqual(port.puts, 0)

    def test_projection_refuses_invalid_or_nonfinite_rates(self):
        for rate in (0, -1, float("inf"), float("nan"), True, "100"):
            with self.assertRaises(ServiceClientError):
                projections([dict(effective_bytes_s=rate, direction="ingress", chunk_raw_bytes=144, storage="test")])


if __name__ == "__main__": unittest.main()

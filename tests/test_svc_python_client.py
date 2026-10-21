"""Python production client exchanges bytes with the production C service."""
import hashlib
import os
from pathlib import Path
import select
import subprocess
import sys
import json
import tempfile
import time
import unittest

from flash_gui.service_client import (OperationJournal, ServiceClient, TransportError,
                                     TransferCancelled)
from flash_gui.service_wire import decode_frame, encode_frame

ROOT = Path(__file__).resolve().parents[1]
BLOB = bytes(range(256)) * 64
SHA = hashlib.sha256(BLOB).hexdigest()


class PipeSerial:
    timeout = 0.1
    write_timeout = 5.0

    def __init__(self, binary, root, drop_put=None):
        self.process = subprocess.Popen([str(binary), str(root)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.is_open = True
        self.drop_put, self.puts, self.commits = drop_put, 0, 0
        self.disconnected = False

    @property
    def in_waiting(self): return 0  # a pipe's available count is not required by the client

    def write(self, data):
        if self.disconnected: raise OSError("disconnected")
        n = os.write(self.process.stdin.fileno(), data)
        if b" p " in data:
            self.puts += 1
            if self.puts == self.drop_put: self.disconnected = True
        if b" schedule.commit " in data: self.commits += 1
        return n

    def read(self, n):
        if self.disconnected: raise OSError("disconnected")
        if not select.select([self.process.stdout], [], [], self.timeout)[0]: return b""
        return os.read(self.process.stdout.fileno(), n)

    def close(self):
        if not self.is_open: return ""
        self.is_open = False
        self.process.stdin.close()
        self.process.wait(timeout=3)
        error = self.process.stderr.read().decode()
        self.process.stdout.close(); self.process.stderr.close()
        if self.process.returncode: raise AssertionError(error)
        return error


class ServicePythonClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "client-peer"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-DSVC_HOST",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I" + str(ROOT / "components/usb_service/include"),
            *(str(ROOT / "components/usb_service" / name) for name in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c", "svc_session.c")),
            str(ROOT / "tests/svc_host/client_peer.c"), "-lcrypto", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls): cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.journal = OperationJournal(self.root / "host", "02:00:00:00:00:01")

    def peer(self, **kwargs):
        port = PipeSerial(self.binary, self.root / "device", **kwargs)
        self.addCleanup(port.close)
        return ServiceClient(port, self.journal), port

    def test_failed_bind_does_not_leak_into_the_next_session_on_the_same_port(self):
        _, port = self.peer()

        def receive(correlation):
            line = bytearray()
            deadline = time.monotonic() + 5
            while not line.endswith(b"\n") and time.monotonic() < deadline:
                line.extend(port.read(1))
            actual, response = decode_frame(bytes(line))
            self.assertEqual(actual, correlation)
            return response

        def request(correlation, body):
            port.write(encode_frame(correlation, body))
            return receive(correlation)

        port.write(b"svc\n")
        self.assertEqual(receive(0)["state"], "ready")
        self.assertEqual(request(1, "svc.bind 2222222222222222")["reason"], "store_changed")
        self.assertEqual(request(2, "svc.result 0000000000000001")["reason"], "store_changed")
        self.assertEqual(request(3, "svc.exit")["state"], "exit")
        port.write(b"svc\n")
        ready = receive(0)
        self.assertEqual(ready["state"], "ready")
        for correlation, body in enumerate(("svc.result 0000000000000001",
                                             "svc.abort 0000000000000001",
                                             "svc.wifi.join 0000000000000001 QTNK b25seXN5bnRoZXRpYw=="), 4):
            self.assertEqual(request(correlation, body)["reason"], "not_bound")
        self.assertEqual(request(7, f"svc.bind {ready['store']}")["state"], "bound")
        self.assertEqual(request(8, "svc.result 0000000000000001")["reason"], "not_found")
        self.assertEqual(request(9, "svc.exit")["state"], "exit")
        self.assertIn("apply_count=0", port.close())

    def test_maximum_real_store_upload_and_exact_schema(self):
        client, port = self.peer()
        result = client.upload_schedule(BLOB, SHA, "a3", "test")
        self.assertEqual((result["state"], result["offset"], result["sha256"], result["applied"]), ("succeeded", 16384, SHA, True))
        self.assertEqual((self.root / "device/stage.bin").read_bytes(), BLOB)
        self.assertEqual(port.puts, 114)
        self.assertIn("apply_count=1", port.close())

    def test_new_device_and_client_process_resume_durable_prefix(self):
        client, port = self.peer(drop_put=2)
        with self.assertRaises(TransportError): client.upload_schedule(BLOB, SHA, "a3", "test")
        self.assertIn("apply_count=0", port.close())
        self.assertEqual((self.root / "device/stage.bin").stat().st_size, 288)
        code = """import sys,json
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from test_svc_python_client import PipeSerial,BLOB,SHA
from flash_gui.service_client import ServiceClient,OperationJournal
root=Path(sys.argv[3]); port=PipeSerial(sys.argv[2],root/'device'); logs=[]
c=ServiceClient(port,OperationJournal(root/'host','02:00:00:00:00:01'))
r=c.upload_schedule(BLOB,SHA,'a3','test',log=logs.append)
print(json.dumps(dict(result=r,puts=port.puts,logs=logs,evidence=port.close())))
"""
        process = subprocess.run([sys.executable, "-c", code, str(ROOT / "tests"), str(self.binary), str(self.root)],
                                 capture_output=True, text=True, timeout=15)
        self.assertEqual(process.returncode, 0, process.stderr)
        result = json.loads(process.stdout)
        self.assertEqual(result["result"]["state"], "succeeded")
        self.assertEqual(result["puts"], 112)
        self.assertTrue(any("288/16384" in line for line in result["logs"]))
        self.assertEqual((self.root / "device/stage.bin").read_bytes(), BLOB)
        self.assertIn("apply_count=1", result["evidence"])

    def test_cancel_after_commit_new_process_reads_result_without_reapply(self):
        client, port = self.peer()
        with self.assertRaises(TransferCancelled):
            client.upload_schedule(BLOB, SHA, "a3", "test", cancelled=lambda: port.commits > 0)
        self.assertIn("apply_count=1", port.close())
        resumed, replacement = self.peer()
        self.assertEqual(resumed.upload_schedule(BLOB, SHA, "a3", "test")["state"], "succeeded")
        self.assertEqual(replacement.commits, 0)
        self.assertIn("apply_count=0", replacement.close())


if __name__ == "__main__": unittest.main()

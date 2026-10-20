# SPDX-License-Identifier: GPL-3.0-or-later
"""Real durable service store: new processes, crash cuts and retained IDs."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


class ServiceStoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name) / "store"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-DSVC_HOST",
            "-I" + str(ROOT / "components/usb_service/include"),
            str(ROOT / "components/usb_service/svc_wire.c"),
            str(ROOT / "components/usb_service/svc_store.c"),
            str(ROOT / "tests/svc_host/store_driver.c"), str(ROOT / "tests/svc_host/store_io.c"),
            *(f"-Wl,--wrap={name}" for name in ("fopen", "fwrite", "fflush", "fsync", "fclose", "rename", "unlink", "ftruncate", "fseek")),
            "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "svc"
        self.addCleanup(self.tmp.cleanup)

    def call(self, *args, directory=None, env=None, crash=False):
        process = subprocess.run([str(self.binary), str(directory or self.root), *map(str, args)],
                                 capture_output=True, text=True, env={**os.environ, **(env or {})})
        if crash and process.returncode == 86:
            return None
        self.assertEqual(process.returncode, 0, process.stderr)
        return json.loads(process.stdout)

    def put(self, offset, data, **kwargs):
        return self.call("put", "1", offset, data.hex(), f"{zlib.crc32(data):08x}", **kwargs)

    def test_admission_replay_conflict_busy_expiry_and_reset(self):
        self.assertEqual(self.call("get", "1")["error"], "not_found")
        self.assertEqual(self.call("admit", "1", 288, 1)["state"], "staging")
        self.assertTrue(self.call("admit", "1", 288, 1)["reused"])
        self.assertEqual(self.call("admit", "1", 288, 2)["error"], "request_conflict")
        self.assertEqual(self.call("admit", "2", 288, 1)["error"], "busy")
        self.assertEqual(self.call("abort", "1")["state"], "aborted")
        self.assertEqual(self.call("abort", "1")["state"], "aborted")
        for n in range(2, 102):
            self.call("admit", f"{n:x}", 144, 1)
            self.call("abort", f"{n:x}")
        self.assertEqual(len(list(self.root.glob("*.rec"))), 8)
        self.assertEqual(self.call("get", "1")["error"], "expired")
        self.assertEqual(self.call("admit", "1", 288, 1)["error"], "expired")
        self.assertEqual(self.call("get", "ff")["error"], "not_found")
        self.assertEqual(self.call("init")["store"], 1)
        shutil.rmtree(self.root)
        self.assertEqual(self.call("init", env={"SVC_INSTANCE": "2"})["store"], 2)
        self.assertEqual(self.call("get", "1")["error"], "not_found")

    def test_chunks_and_terminal_sequences(self):
        self.call("admit", "1", 291, 1)
        a, b = b"a" * 144, b"b" * 144
        self.assertEqual(self.put(0, a)["offset"], 144)
        self.assertEqual(self.put(0, a)["offset"], 144)
        self.assertEqual(self.put(0, b)["error"], "offset_conflict")
        self.assertEqual(self.put(145, b)["error"], "offset_gap")
        self.assertEqual(self.put(144, b)["offset"], 288)
        self.assertEqual(self.put(0, a)["offset"], 288)
        self.assertEqual(self.put(288, b"abc")["offset"], 291)
        self.assertEqual((self.root / "stage.bin").read_bytes(), a + b + b"abc")
        self.call("finish", "1")
        self.assertEqual(self.put(0, a)["error"], "operation_complete")
        self.assertEqual(self.call("abort", "1")["error"], "operation_complete")

    def test_chunk_errors_and_space_admission(self):
        self.assertEqual(self.call("admit", "1", 144, 1, env={"SVC_FREE": "65536"})["error"], "no_space")
        self.assertEqual(self.call("init")["highest"], 0)
        self.assertEqual(self.call("admit", "1", 16385, 1)["error"], "size_limit")
        self.call("admit", "1", 288, 1)
        self.assertEqual(self.call("put", "1", 0, (b"a" * 144).hex(), "0")["error"], "chunk_crc")
        self.assertEqual(self.put(0, b"abc")["error"], "offset_conflict")
        self.assertEqual(self.put(0, b"a" * 145)["error"], "size_limit")

    def test_chunk_crash_matrix(self):
        self.call("admit", "1", 288, 1)
        self.put(0, b"a" * 144)
        snapshot = Path(self.tmp.name) / "snapshot"
        shutil.copytree(self.root, snapshot)
        observed = set()
        for cut in range(1, 18):
            case = Path(self.tmp.name) / f"cut-{cut}"
            shutil.copytree(snapshot, case)
            self.put(144, b"b" * 144, directory=case, env={"SVC_CUT": str(cut)}, crash=True)
            resumed = self.call("get", "1", directory=case)
            self.assertIn(resumed["offset"], (144, 288), (cut, resumed))
            observed.add(resumed["offset"])
            self.assertEqual((case / "stage.bin").read_bytes(),
                             b"a" * 144 + (b"b" * 144 if resumed["offset"] == 288 else b""))
            self.put(144, b"b" * 144, directory=case)
            self.assertEqual((case / "stage.bin").read_bytes(), b"a" * 144 + b"b" * 144)
        self.assertEqual(observed, {144, 288})

    def test_admission_crash_matrix_never_reuses_reserved_id(self):
        self.call("init")
        snapshot = Path(self.tmp.name) / "empty"
        shutil.copytree(self.root, snapshot)
        for cut in range(1, 20):
            case = Path(self.tmp.name) / f"cut-{cut}"
            shutil.copytree(snapshot, case)
            self.call("admit", "1", 144, 1, directory=case, env={"SVC_CUT": str(cut)}, crash=True)
            result = self.call("get", "1", directory=case)
            self.assertIn(result.get("error", "staging"), ("expired", "not_found", "staging"), (cut, result))
            if result.get("error") == "expired":
                self.assertEqual(self.call("admit", "1", 144, 1, directory=case)["error"], "expired")

    def test_enospc_and_io_error_matrix_preserves_acknowledged_prefix(self):
        self.call("admit", "1", 288, 1)
        self.put(0, b"a" * 144)
        snapshot = Path(self.tmp.name) / "snapshot"
        shutil.copytree(self.root, snapshot)
        for operation, indices in (("open", (1, 2)), ("write", (1, 2)), ("flush", (1, 2)),
                                   ("sync", (1, 2)), ("rename", (1,)), ("seek", (1,))):
            for index in indices:
                for eio in (False, True):
                    with self.subTest(operation=operation, index=index, eio=eio):
                        case = Path(self.tmp.name) / f"{operation}-{index}-{eio}"
                        shutil.copytree(snapshot, case)
                        env = {"SVC_IO_OP": operation, "SVC_IO_AT": str(index)}
                        if eio: env["SVC_IO_EIO"] = "1"
                        failed = self.put(144, b"b" * 144, directory=case, env=env)
                        self.assertIn(failed["error"], ("no_space", "storage_error", "corrupt_store"))
                        result = self.call("get", "1", directory=case)
                        self.assertEqual(result["offset"], 144)
                        self.assertEqual((case / "stage.bin").read_bytes(), b"a" * 144)
                        self.assertEqual(self.put(144, b"b" * 144, directory=case)["offset"], 288)

    def test_final_result_crash_and_gc_matrix(self):
        for op in range(1, 9):
            self.call("admit", f"{op:x}", 144, 1)
            self.call("abort", f"{op:x}")
        self.call("admit", "9", 144, 1)
        snapshot = Path(self.tmp.name) / "gc-snapshot"
        shutil.copytree(self.root, snapshot)
        states = set()
        for cut in range(1, 11):
            case = Path(self.tmp.name) / f"gc-cut-{cut}"
            shutil.copytree(snapshot, case)
            self.call("finish", "9", directory=case, env={"SVC_CUT": str(cut)}, crash=True)
            result = self.call("get", "9", directory=case)
            states.add(result["state"])
            self.assertIn(result["state"], ("staging", "succeeded"))
            if result["state"] == "succeeded":
                self.assertEqual(len(list(case.glob("*.rec"))), 8)
                self.assertEqual(self.call("get", "1", directory=case)["error"], "expired")
            self.assertEqual(self.call("admit", "9", 144, 1, directory=case)["state"], result["state"])
        self.assertEqual(states, {"staging", "succeeded"})
        case = Path(self.tmp.name) / "gc-unlink-failure"
        shutil.copytree(snapshot, case)
        self.assertEqual(self.call("finish", "9", directory=case, env={"SVC_IO_OP": "unlink"})["error"], "no_space")
        self.assertEqual(self.call("get", "9", directory=case)["state"], "succeeded")
        self.assertEqual(len(list(case.glob("*.rec"))), 8)

    def test_abort_cleanup_and_truncate_errors_recover_without_reexecution(self):
        self.call("admit", "1", 288, 1)
        self.put(0, b"a" * 144)
        result = self.call("abort", "1", env={"SVC_IO_OP": "unlink"})
        self.assertEqual(result["error"], "no_space")
        self.assertEqual(self.call("get", "1")["state"], "aborted")
        self.assertEqual(self.call("admit", "1", 288, 1)["state"], "aborted")
        self.call("admit", "2", 288, 1)
        (self.root / "stage.bin").write_bytes(b"unacknowledged")
        self.assertEqual(self.call("get", "2", env={"SVC_IO_OP": "truncate"})["error"], "no_space")
        self.assertEqual(self.call("get", "2")["offset"], 0)
        self.assertEqual((self.root / "stage.bin").read_bytes(), b"")

    def test_result_close_errors_cannot_ack_false_success(self):
        self.call("admit", "1", 288, 1)
        # The open path first closes the header and record reads; fail the
        # chunk or replacement write close by tracing all reachable indices.
        for index in range(1, 15):
            case = Path(self.tmp.name) / f"close-{index}"
            shutil.copytree(self.root, case)
            result = self.put(0, b"a" * 144, directory=case, env={"SVC_IO_OP": "close", "SVC_IO_AT": str(index)})
            resumed = self.call("get", "1", directory=case)
            if "error" not in result: self.assertEqual(resumed["offset"], 144)
            else: self.assertIn(resumed["offset"], (0, 144))
            self.assertEqual(self.put(0, b"a" * 144, directory=case)["offset"], 144)

    def test_corrupt_store_never_reinitializes(self):
        self.call("admit", "1", 144, 1)
        original = (self.root / "store").read_bytes()
        record = self.root / "0000000000000001.rec"
        record.write_bytes(record.read_bytes()[:-1])
        self.assertEqual(self.call("get", "1")["error"], "corrupt_store")
        self.assertEqual((self.root / "store").read_bytes(), original)
        record.unlink()
        (self.root / "store").write_bytes(b"unsupported")
        self.assertEqual(self.call("admit", "2", 144, 1)["error"], "corrupt_store")
        self.assertEqual((self.root / "store").read_bytes(), b"unsupported")

    def test_identifier_exhaustion_never_wraps_or_admits_again(self):
        self.call("admit", "ffffffffffffffff", 144, 1)
        self.call("abort", "ffffffffffffffff")
        self.assertEqual(self.call("admit", "1", 144, 1)["error"], "id_exhausted")
        self.assertEqual(self.call("init")["highest"], 2**64 - 1)

    def test_missing_acknowledged_data_is_corruption(self):
        self.call("admit", "1", 144, 1)
        self.put(0, b"a" * 144)
        (self.root / "stage.bin").write_bytes(b"a")
        self.assertEqual(self.call("get", "1")["error"], "corrupt_store")


if __name__ == "__main__":
    unittest.main()

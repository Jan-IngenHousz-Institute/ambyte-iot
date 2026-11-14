"""Remote SD log reads (log_list / log_read): the PRODUCTION log_command.c with
the pinned ESP-IDF cJSON, against a real temporary log directory. Name
validation and bounds (.., /, unknown name, negative offset, past EOF, length
cap), SD-state refusals, maintenance-gate and busy refusals, read-only proof.
Run with CC=clang."""
import base64
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _cjson():
    # Same SDK the firmware builds with; CI (AMBYTE_REQUIRE_PINNED_IDF=1) fails
    # rather than skips when it is missing.
    from pinned_idf import pinned_idf
    return pinned_idf() / "components/json/cJSON"


class LogCommand(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("CC") or shutil.which("clang")
        if not cc or not shutil.which(cc):
            raise unittest.SkipTest("clang not available (set CC=clang)")
        cls.tmp = tempfile.TemporaryDirectory(prefix="log-command-")
        cls.bin = Path(cls.tmp.name) / "log-command"
        cjson = _cjson()
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE",
                        "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
                        f"-I{ROOT / 'components/command_router/include'}", f"-I{cjson}",
                        str(ROOT / "tests/log_command_host.c"),
                        str(ROOT / "components/command_router/log_command.c"),
                        str(cjson / "cJSON.c"), "-lm", "-o", str(cls.bin)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="logs-", dir=self.tmp.name))
        self.main = bytes((i * 7 + 13) % 95 + 32 for i in range(20000))   # 20000 printable bytes
        (self.dir / "ambyte.log").write_bytes(self.main)
        (self.dir / "ambyte.1.log").write_bytes(b"rotated\n" * 10)
        (self.dir / "binary.log").write_bytes(bytes(range(256)))
        (self.dir / "empty.log").write_bytes(b"")
        (self.dir / "subdir").mkdir()
        (self.dir / ".hidden").write_bytes(b"x")
        (self.dir / "we ird.log").write_bytes(b"x")

    def run_case(self, case):
        r = subprocess.run([self.bin, str(self.dir), case], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return [json.loads(l[len("REPLY "):]) for l in r.stdout.splitlines() if l.startswith("REPLY ")]

    def test_pure_helpers(self):
        r = subprocess.run([self.bin, str(self.dir), "pure"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("PURE ok", r.stdout)

    def test_list_shows_readable_files_sorted(self):
        (reply,) = self.run_case("list")
        self.assertEqual(reply["type"], "log_list_result")
        self.assertEqual(reply["id"], 'l-"1')
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["dir"], str(self.dir))
        self.assertEqual(reply["files"], [{"name": "ambyte.1.log", "size": 80}, {"name": "ambyte.log", "size": 20000},
                                          {"name": "binary.log", "size": 256}, {"name": "empty.log", "size": 0}])
        self.assertEqual(reply["omitted"], 3)                   # subdir, .hidden, "we ird.log"

    def test_reads_offsets_tail_eof_cap_and_errors(self):
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.dir.iterdir() if p.is_file()}
        replies = {r["id"]: r for r in self.run_case("reads")}

        def data(r):
            self.assertEqual(r["encoding"], "base64")
            raw = base64.b64decode(r["data"], validate=True)
            self.assertEqual(len(raw), r["length"])
            return raw

        r1 = replies["r1"]
        self.assertEqual((r1["ok"], r1["offset"], r1["length"], r1["size"], r1["eof"]), (True, 0, 4096, 20000, False))
        self.assertEqual(data(r1), self.main[:4096])
        self.assertEqual(data(replies["r2"]), self.main[5000:5100])
        r3 = replies["r3"]                                       # negative offset = tail
        self.assertEqual((r3["offset"], r3["length"], r3["eof"]), (19700, 300, True))
        self.assertEqual(data(r3), self.main[-300:])
        r4 = replies["r4"]                                       # length clamped to 8192
        self.assertEqual((r4["length"], r4["eof"]), (8192, False))
        self.assertEqual(data(r4), self.main[:8192])
        self.assertLess(len(json.dumps(r4)), 12 * 1024)          # bounded wire size
        r5 = replies["r5"]
        self.assertEqual((r5["ok"], r5["length"], r5["eof"], r5["data"]), (True, 0, True, ""))
        r6 = replies["r6"]
        self.assertEqual((r6["ok"], r6["detail"], r6["size"]), (False, "offset_past_eof", 20000))
        self.assertNotIn("data", r6)
        r7 = replies["r7"]                                       # tail longer than the file
        self.assertEqual((r7["offset"], r7["eof"]), (0, True))
        self.assertEqual(data(r7), b"rotated\n" * 10)
        self.assertEqual(data(replies["r8"]), bytes(range(256)))  # binary-safe
        self.assertEqual((replies["r9"]["ok"], replies["r9"]["size"], replies["r9"]["eof"]), (True, 0, True))
        self.assertEqual((replies["r10"]["ok"], replies["r10"]["detail"]), (False, "not_found"))
        # A directory entry: FATFS refuses the fopen (open_failed); POSIX opens it.
        self.assertIn((replies["r11"]["ok"], replies["r11"]["detail"]), {(False, "not_a_file"), (False, "open_failed")})
        after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.dir.iterdir() if p.is_file()}
        self.assertEqual(before, after, "log_read must be read-only")

    def test_invalid_requests_never_reach_the_card(self):
        replies = self.run_case("invalid")
        details = [(r.get("id"), r["detail"]) for r in replies]
        self.assertEqual(details, [
            ("b1", "invalid_name"), ("b2", "invalid_name"), ("b3", "invalid_name"), ("b4", "invalid_name"),
            ("b5", "invalid_name"), ("b6", "invalid_name"), ("b7", "invalid_offset"), ("b8", "invalid_offset"),
            ("b9", "invalid_length"), ("b10", "invalid_length"), ("b11", "invalid_offset"),
            ("", "invalid_id"), ("", "invalid_id")])
        self.assertTrue(all(r["ok"] is False for r in replies))

    def test_sd_state_maintenance_and_busy_refusals(self):
        replies = self.run_case("refusals")
        self.assertEqual([(r["type"], r["detail"]) for r in replies], [
            ("log_read_result", "sd_parked"), ("log_list_result", "sd_absent"),
            ("log_read_result", "sd_io_lost"), ("log_read_result", "sd_busy"),
            ("log_read_result", "maintenance_active"),
            ("log_read_result", "busy"), ("log_read_result", "ok"),
            ("log_read_result", "queue_unavailable"), ("log_read_result", "ok")])


if __name__ == "__main__":
    unittest.main()

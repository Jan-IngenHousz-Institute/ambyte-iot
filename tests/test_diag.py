"""Remote diagnostics (components/diag): reset-reason table, crash-summary JSON,
and the ESP_LOGE capture ring / rate limiter / hook / ambyte.log/1 renderer.

Compiles the PRODUCTION diag sources with tests/diag_host/diag_host.c, which
only supplies the four port facts (lock, clock, ISR flag, task id) as pthread
equivalents. ASan+UBSan for every scenario; the concurrency scenario also runs
under ThreadSanitizer. Run with CC=clang.
"""
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/diag_host"
DIAG = ROOT / "components/diag"
SOURCES = [HOST / "diag_host.c", DIAG / "diag_reset.c", DIAG / "diag_crash.c",
           DIAG / "diag_json.c", DIAG / "diag_log.c"]

MIN_BURST, MIN_REFILL_S = 20, 6
HOUR_BURST, HOUR_REFILL_S = 60, 15


def _cc():
    cc = os.environ.get("CC") or shutil.which("clang")
    if not cc or not shutil.which(cc):
        raise unittest.SkipTest("clang not available (set CC=clang)")
    return cc


def _build(out, sanitize):
    cmd = [_cc(), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-DDIAG_LOG_HOST",
           "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fno-omit-frame-pointer", f"-fsanitize={sanitize}",
           "-pthread", f"-I{HOST / 'include'}", f"-I{DIAG / 'include'}", f"-I{DIAG}",
           *(str(s) for s in SOURCES), "-o", str(out)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)


class DiagHost(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="diag-host-")
        cls.bin = Path(cls.tmp.name) / "diag"
        cls.tsan = Path(cls.tmp.name) / "diag-tsan"
        try:
            _build(cls.bin, "address,undefined")
        except subprocess.CalledProcessError as e:
            raise RuntimeError(e.stdout + e.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case, binary=None):
        r = subprocess.run([binary or self.bin, case], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def records(self, out):
        recs = [json.loads(line[len("RECORD "):]) for line in out.splitlines() if line.startswith("RECORD ")]
        for line in out.splitlines():
            if line.startswith("RECORD "):
                self.assertLess(len(line[len("RECORD "):].encode()), 4096, "a record must stay under 4 KiB")
        for r in recs:
            self.assertEqual(list(r)[:2], ["schema", "measure_id"])   # canonical routing prefix
            self.assertEqual(r["schema"], "ambyte.log/1")
            self.assertEqual(r["tag"], "LOG")
            self.assertEqual(set(r), {"schema", "measure_id", "device", "tag", "time", "boot_count",
                                      "lines", "dropped", "rate_limited"})
            for line in r["lines"]:
                self.assertEqual(set(line), {"uptime_ms", "utc_ms", "level", "tag", "msg", "repeat"})
                self.assertEqual(line["level"], "E")
                self.assertGreaterEqual(line["repeat"], 1)
        return recs

    # ── A: reset reason ─────────────────────────────────────────────────
    def test_reset_reason_mapping_matches_ticket_enum(self):
        got = json.loads(self.run_case("reset"))
        expected = ["unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "other_wdt",
                    "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup"]
        for value, name in enumerate(expected):
            self.assertEqual(got[str(value)], name, value)
        # Out-of-range values (a newer IDF) read "unknown", never a neighbour.
        self.assertEqual((got["16"], got["17"], got["-1"]), ("unknown",) * 3)

    def test_host_enum_matches_pinned_idf(self):
        from pinned_idf import pinned_idf
        hdr = pinned_idf() / "components/esp_system/include/esp_system.h"
        def names(text):
            body = re.search(r"typedef enum \{(.*?)\} esp_reset_reason_t;", text, re.S).group(1)
            return re.findall(r"^\s*(ESP_RST_\w+)", body, re.M)
        self.assertEqual(names((HOST / "include/esp_system.h").read_text()), names(hdr.read_text()))

    def test_one_reset_table(self):
        # ambit_flash's boot-sync log line spells reasons through diag_reset,
        # not a private copy (ticket: no two divergent tables).
        src = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        self.assertIn("diag_reset_reason_name(rr)", src)
        self.assertNotIn("reset_reason_str", src)

    # ── B: crash summary ────────────────────────────────────────────────
    def test_crash_summary_json(self):
        out = dict(line.split(" ", 1) for line in self.run_case("crash").splitlines())
        self.assertEqual(json.loads(out["ABSENT"]), {"present": False})
        self.assertEqual(json.loads(out["NULL"]), {"present": False})
        self.assertEqual(json.loads(out["ABSENT_ERR"]), {"present": False, "error": "no_partition"})
        present = json.loads(out["PRESENT"])
        self.assertEqual(present, {"present": True, "id": "0000a1b2c3d4e5f6", "first_seen_boot": 4294967295,
                                   "app_elf_sha256": "0123456789abcdef", "task": "mqtt_task",
                                   "pc": "0x4200abcd", "exc_cause": 29, "exc_vaddr": "0xdeadbeef",
                                   "backtrace": ["0x40370000", "0x40370001", "0x40370002"],
                                   "bt_corrupted": False})
        worst = json.loads(out["PRESENT_WORST"])
        self.assertEqual(len(worst["backtrace"]), 16)          # depth 40 clamped
        self.assertTrue(worst["bt_corrupted"])
        corrupt = json.loads(out["CORRUPT"])
        self.assertEqual(corrupt, {"present": True, "id": "000012349abcdef0", "first_seen_boot": 3,
                                   "error": "ESP_ERR_INVALID_CRC"})

    # ── C: capture, ring, rate limit ────────────────────────────────────
    def test_parse_truncate_escape_and_level_filter(self):
        (rec,) = self.records(self.run_case("parse"))
        lines = rec["lines"]
        self.assertEqual([(l["tag"], l["msg"]) for l in lines[:2]],
                         [("sd_card", "mount failed: ESP_ERR_TIMEOUT"),
                          ("", "E something without the IDF prefix")])
        self.assertEqual(lines[2]["tag"], "long")
        self.assertEqual(lines[2]["msg"], "x" * 160)
        self.assertEqual(lines[3]["msg"], "y" * 159)            # em dash cut whole at the cap
        self.assertEqual(lines[4]["msg"], 'quote" back\\ tab\t bell\x07 bad� end')
        self.assertEqual(len(lines), 5)                         # W and I lines are not captured
        self.assertTrue(all(l["utc_ms"] is None for l in lines))
        self.assertEqual(rec["boot_count"], 7)

    def test_coalescing_is_exactly_once_across_seal_and_failed_store(self):
        out = self.run_case("coalesce")
        recs = self.records(out)
        self.assertEqual([[l["repeat"] for l in r["lines"]] for r in recs], [[100, 2], [1]])
        self.assertIn("STATS captured=3 repeats=100 records=2", out)

    def test_full_ring_drops_without_spending_tokens(self):
        out = self.run_case("ring_full")
        self.assertIn("STATS buffered=24 dropped=1 rate_limited=1", out)
        (rec,) = self.records(out)
        self.assertEqual((rec["dropped"], rec["rate_limited"]), (1, 1))
        left = 24 - len(rec["lines"])                           # lines that did not fit this record
        # The line dropped on a full ring left its token: the next line gets in.
        self.assertIn(f"AFTER buffered={left + 1} dropped=0 rate_limited=0", out)

    def test_recursion_and_isr_are_dropped_and_counted_but_forwarded(self):
        out = self.run_case("recursion")
        self.assertIn("STATS buffered=1 dropped=1 forwarded=2", out)
        (rec,) = self.records(out)
        self.assertEqual([l["tag"] for l in rec["lines"]], ["outer"])
        self.assertEqual(rec["dropped"], 1)
        self.assertIn("ISR buffered=0 dropped=1 forwarded=3", out)

    def test_pre_sync_lines_carry_null_utc_and_record_interval(self):
        out = self.run_case("clock")
        now_us = int(re.search(r"NOW_US (\d+)", out).group(1))
        (rec,) = self.records(out)
        before, after = rec["lines"]
        self.assertIsNone(before["utc_ms"])
        # captured 3 s before the drain, drain clock = 1763100000000
        self.assertEqual(after["utc_ms"], 1763100000000 - 3000)
        self.assertEqual(after["uptime_ms"], (now_us - 3_000_000) // 1000)
        self.assertIn("SOON 0", out)                            # < 60 s after the last record
        self.assertIn("LATER 1", out)

    def test_worst_case_lines_split_across_records_under_4kib(self):
        out = self.run_case("size")
        recs = self.records(out)
        self.assertEqual(sum(len(r["lines"]) for r in recs), 24)
        self.assertEqual(int(re.search(r"RECORDS (\d+)", out).group(1)), len(recs))
        self.assertGreater(len(recs), 1)
        firsts = [l["msg"][0] for r in recs for l in r["lines"]]
        self.assertEqual(firsts, [chr(ord("A") + i) for i in range(24)])   # order kept, nothing lost

    def test_token_buckets_cap_every_window(self):
        out = self.run_case("bucket")
        admits = [int(m) for m in re.findall(r"ADMIT (\d+)", out)]
        self.assertGreater(len(admits), 600)
        j = 0
        for i, t in enumerate(admits):                           # any 60 s window ≤ 30
            while admits[j] <= t - 60_000_000:
                j += 1
            self.assertLessEqual(i - j + 1, 30, f"minute window ending {t}")
        j = 0
        for i, t in enumerate(admits):                           # any hour ≤ 300
            while admits[j] <= t - 3_600_000_000:
                j += 1
            self.assertLessEqual(i - j + 1, 300, f"hour window ending {t}")
        # And the limiter is not stingier than designed: burst + 4/min sustained.
        self.assertGreaterEqual(len(admits), HOUR_BURST + 3 * 3600 // HOUR_REFILL_S - 2)

    def _check_concurrent(self, out):
        recs = self.records(out)
        total = int(re.search(r"TOTAL (\d+)", out).group(1))
        accounted = sum(sum(l["repeat"] for l in r["lines"]) + r["dropped"] + r["rate_limited"] for r in recs)
        self.assertEqual(accounted, total, "every E line in exactly one record or count")
        self.assertIn(f"forwarded={total}", out)                # and every line reached the sinks

    def test_concurrent_producers_exactly_once(self):
        self._check_concurrent(self.run_case("concurrent"))

    def test_concurrent_producers_thread_sanitizer(self):
        try:
            _build(self.tsan, "thread")
        except subprocess.CalledProcessError as e:
            self.skipTest(f"TSan unavailable: {e.stderr[-300:]}")
        r = subprocess.run([self.tsan, "concurrent"], capture_output=True, text=True, timeout=300)
        if r.returncode != 0 and "unexpected memory mapping" in r.stderr:
            # Older TSan runtimes cannot map their shadow under high mmap ASLR
            # entropy (some CI kernels). ASan still covers this scenario above.
            self.skipTest("TSan runtime incompatible with this kernel's mmap layout")
        self.assertEqual(r.returncode, 0, r.stderr)
        self._check_concurrent(r.stdout)

    # ── wiring that host builds cannot see ──────────────────────────────
    def test_hook_chain_and_drainer_wiring(self):
        app = (ROOT / "main/app_main.c").read_text()
        # sd_logger first, diag second: esp_log -> diag -> sd_logger -> console.
        self.assertLess(app.index("sd_logger_init()"), app.index("diag_log_install()"))
        self.assertLess(app.index("app_init_nvs()"), app.index("diag_boot_init()"))
        sync = (ROOT / "components/sync_runner/sync_runner.c").read_text()
        self.assertIn("cmd_store_log_event()", sync)
        # Everything up to the drain section runs on the logging task: code
        # (comments stripped) must not reach event_log, the heap, a blocking
        # lock or the libc clock (IDF's time() takes a _lock).
        hook = (DIAG / "diag_log.c").read_text().split("/* ── drain (task context)")[0]
        code = re.sub(r"/\*.*?\*/", "", hook, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        for forbidden in ("event_log", "malloc", "calloc", "xSemaphore", "time(", "gettimeofday", "vTaskDelay"):
            self.assertNotIn(forbidden, code, forbidden)
        self.assertIn('"{\\"schema\\":\\"ambyte.log/1\\""', (ROOT / "components/payload_codec/payload_v3.c").read_text())


if __name__ == "__main__":
    unittest.main()

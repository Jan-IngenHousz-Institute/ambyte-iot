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
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/diag_host"
DIAG = ROOT / "components/diag"
SOURCES = [HOST / "diag_host.c", DIAG / "diag_reset.c", DIAG / "diag_crash.c", DIAG / "diag_fmt.c",
           DIAG / "diag_json.c", DIAG / "diag_log.c", DIAG / "diag_log_hook.c"]
HOOK_OBJECTS = (DIAG / "diag_log_hook.c", DIAG / "diag_fmt.c")

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
                                      "lines", "dropped", "rate_limited", "unsupported_fmt"})
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
        self.assertIn('"{\\"schema\\":\\"ambyte.log/1\\""', (ROOT / "components/payload_codec/payload_v3.c").read_text())


    # ── review finding 2: the hook never reaches newlib printf / the heap ──
    def test_formatter_matches_libc_for_integers_and_strings(self):
        rows = [l.split("\t") for l in self.run_case("fmt").splitlines()]
        ints = [r for r in rows if r[0] == "INT"]
        self.assertGreater(len(ints), 8)
        for kind, fmt, ours, libc, unsupported in ints:
            with self.subTest(fmt=fmt):
                self.assertEqual(ours, libc)
                self.assertEqual(unsupported, "0")

    def test_formatter_renders_floats_without_allocation(self):
        rows = [l.split("\t") for l in self.run_case("fmt").splitlines()]
        for kind, fmt, ours, libc, unsupported in (r for r in rows if r[0] == "FLT"):
            with self.subTest(fmt=fmt, ours=ours, libc=libc):
                self.assertEqual(unsupported, "0")
                if "e" in ours and "e" not in libc and "E" not in libc:
                    # 1e300 does not fit the integer path: e-form, same value.
                    self.assertAlmostEqual(float(ours) / 1e300, 1.0, places=9)
                    continue
                # Same shape (width, sign, padding, exponent form) and value to
                # the printed precision.
                self.assertEqual(len(ours), len(libc))
                num = lambda t: float(re.sub(r"[^0-9eE+.\-]", "", t.split("|")[0]) or "0")
                self.assertAlmostEqual(num(ours), num(libc), delta=abs(num(libc)) * 1e-9 + 10 ** -(
                    len(libc.split(".")[-1].split("e")[0].split("E")[0].rstrip("|°C ")) if "." in libc else 0))
        special = next(r for r in rows if r[0] == "SPECIAL")
        self.assertEqual(special[2].lower(), special[3].lower())

    def test_formatter_unsupported_conversions_are_counted_and_visible(self):
        odd = {r[1]: (r[2], r[3]) for r in (l.split("\t") for l in self.run_case("fmt").splitlines()) if r[0] == "ODD"}
        self.assertEqual(odd["a=%a b=%d"], ("a=<%a> b=7", "1"))
        self.assertEqual(odd["n=%n then %d"], ("n= then 9", "1"))
        self.assertEqual(odd["w=%ls %lc end %d"], ("w=<%ls> <%lc> end 3", "2"))
        self.assertEqual(odd["bad %q %d"], ("bad %q<?>", "1"))     # stops: arg sizes unknowable
        self.assertEqual(odd["trailing %"], ("trailing %<?>", "1"))
        self.assertIn("TRUNC ok", self.run_case("fmt_trunc"))

    def test_float_error_line_through_the_hook(self):
        out = self.run_case("hook_fmt")
        self.assertIn("STATS unsupported=2 buffered=3", out)
        (rec,) = self.records(out)
        self.assertEqual([l["msg"] for l in rec["lines"]],
                         ["temperature 85.13 out of range (3)", "count= done", "hex float <%a>"])
        self.assertEqual(rec["unsupported_fmt"], 2)

    def _host_objects(self, sources, extra=()):
        cc = _cc()
        objs = []
        for src in sources:
            obj = Path(self.tmp.name) / (Path(src).stem + ".o")
            # No sanitizers: their runtime hooks would hide what the code itself calls.
            subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DDIAG_LOG_HOST", "-O1", "-c",
                            f"-I{DIAG / 'include'}", f"-I{DIAG}", *extra, str(src), "-o", str(obj)],
                           check=True, capture_output=True, text=True)
            objs.append(obj)
        return objs

    def test_hook_objects_reference_no_allocator_or_printf(self):
        checker = ROOT / "tools/check_diag_hook_symbols.py"
        objs = self._host_objects(HOOK_OBJECTS)
        r = subprocess.run([sys.executable, str(checker), *map(str, objs)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # The checker is not vacuous: the drain object (newlib vsnprintf via
        # diag_json) and a hook that formats with vsnprintf / allocates fail it.
        bad = Path(self.tmp.name) / "bad_hook.c"
        bad.write_text('#include <stdarg.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
                       'void *g_keep;\nint bad_capture(const char *f, va_list ap) { char b[64]; g_keep = malloc(4);'
                       ' return vsnprintf(b, sizeof b, f, ap); }\n')
        for obj, needle in ((self._host_objects([bad])[0], "vsnprintf"),
                            (self._host_objects([DIAG / "diag_json.c"])[0], "vsnprintf")):
            r = subprocess.run([sys.executable, str(checker), str(obj)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1, r.stdout)
            self.assertIn(needle, r.stdout)
        self.assertIn("malloc", subprocess.run([sys.executable, str(checker), str(Path(self.tmp.name) / "bad_hook.o")],
                                               capture_output=True, text=True).stdout)

    def test_firmware_hook_objects_when_built(self):
        objs = [ROOT / ".pio/build/esp32-s3-devkitm-1/components/diag" / f"{Path(s).name}.o" for s in HOOK_OBJECTS]
        if not all(o.exists() for o in objs):
            self.skipTest("firmware not built (CI runs tools/check_diag_hook_symbols.py after pio run)")
        r = subprocess.run([sys.executable, str(ROOT / "tools/check_diag_hook_symbols.py"), *map(str, objs)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_ci_runs_the_target_symbol_check(self):
        pr = (ROOT / ".github/workflows/pr.yml").read_text()
        self.assertIn("tools/check_diag_hook_symbols.py", pr)
        self.assertIn("components/diag/diag_log_hook.c.o", pr)



class DiagBoot(unittest.TestCase):
    """The production diag_boot.c (review finding 3): boot counter, coredump
    identity and first_seen_boot across "boots" (one process each), against a
    64 KiB partition file and a file-backed NVS."""

    PART = 0x10000

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="diag-boot-")
        cls.bin = Path(cls.tmp.name) / "boot"
        cmd = [_cc(), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-DDIAG_LOG_HOST",
               "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
               f"-I{HOST / 'boot_include'}", f"-I{HOST / 'include'}", f"-I{DIAG / 'include'}", f"-I{DIAG}",
               str(HOST / "boot_host.c"), *(str(DIAG / f) for f in (
                   "diag_boot.c", "diag_crash.c", "diag_fmt.c", "diag_json.c", "diag_log.c",
                   "diag_log_hook.c", "diag_reset.c")), "-o", str(cls.bin)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(dir=self.tmp.name))
        self.nvs = self.dir / "nvs"

    def image(self, payload, size=None):
        """A dump the way IDF writes it: size word, payload, CRC32 over [0, size-4)."""
        import struct
        import zlib
        size = size or 8 + len(payload)
        body = struct.pack("<I", size) + payload
        body = body[:size - 4].ljust(size - 4, b"\0")
        img = body + struct.pack("<I", zlib.crc32(body))
        return img.ljust(self.PART, b"\xff")

    def boot(self, flash, reset=4, extra=()):
        part = self.dir / "part"
        if flash is None:
            arg = "-"
        else:
            part.write_bytes(flash)
            arg = str(part)
        r = subprocess.run([self.bin, arg, str(self.nvs), str(reset), *extra],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    @staticmethod
    def payload(tail=b"", task=b"mqtt_task"):
        import struct
        head = struct.pack("<I", 0x4200ABCD) + task.ljust(16, b"\0") + struct.pack("<IIIII", 28, 0x10, 1, 2, 3)
        return (head + b"\x5a" * 4000)[:4000 - len(tail)] + tail

    def test_boot_counter_and_reset_reason(self):
        blank = b"\xff" * self.PART
        self.assertEqual([self.boot(blank)["boot_count"] for _ in range(3)], [1, 2, 3])
        out = self.boot(blank, reset=9)
        self.assertEqual((out["boot_count"], out["reset_reason"], out["crash"]), (4, "brownout", {"present": False}))
        broken = self.boot(blank, extra=("nvs-broken",))
        self.assertEqual(broken["boot_count"], 0)                    # unknown, never a fresh 1

    def test_valid_dump_summary_and_first_seen_survives_boots(self):
        img = self.image(self.payload())
        first = self.boot(img)
        c = first["crash"]
        self.assertEqual((c["present"], c["task"], c["pc"], c["exc_cause"], c["first_seen_boot"]),
                         (True, "mqtt_task", "0x4200abcd", 28, 1))
        self.assertEqual(c["backtrace"], ["0x00000001", "0x00000002", "0x00000003"])
        self.assertEqual(c["app_elf_sha256"], "0123456789abcdef")
        again = self.boot(img)
        self.assertEqual((again["boot_count"], again["crash"]["first_seen_boot"], again["crash"]["id"]),
                         (2, 1, c["id"]))                           # same dump: still first seen on boot 1
        self.assertTrue(c["id"].startswith("%08x" % 4008))           # identity = size + stored checksum

    def test_same_size_dumps_differing_only_at_the_end_get_new_identity(self):
        # The reviewer's collision: equal size, payloads differ only in the
        # last 28 bytes before the CRC. The old last-32-bytes hash collided.
        a = self.image(self.payload(tail=b"A" * 28))
        b = self.image(self.payload(tail=b"B" * 28))
        self.assertEqual(len(a), len(b))
        first = self.boot(a)["crash"]
        second = self.boot(b)["crash"]
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual((first["first_seen_boot"], second["first_seen_boot"]), (1, 2))   # a NEW crash
        self.assertEqual(self.boot(b)["crash"]["first_seen_boot"], 2)

    def test_corrupt_dump_keeps_identity_and_is_never_parsed(self):
        img = bytearray(self.image(self.payload()))
        good = self.boot(bytes(img))["crash"]
        img[100] ^= 0xFF                                             # bit rot after the write
        bad = self.boot(bytes(img))
        self.assertEqual(bad["summary_calls"], 0)                    # image_check before get_summary
        self.assertEqual(bad["crash"], {"present": True, "id": good["id"], "first_seen_boot": 1,
                                        "error": "ESP_ERR_INVALID_CRC"})

    def test_partial_and_garbage_headers_get_a_fallback_identity(self):
        import struct
        garbage = bytearray(b"\x11" * self.PART)
        garbage[0:4] = struct.pack("<I", 0x00200000)                 # larger than the partition
        one = self.boot(bytes(garbage))["crash"]
        self.assertEqual((one["present"], one["error"], one["first_seen_boot"]), (True, "invalid_size", 1))
        self.assertTrue(one["id"].startswith("ffffffff"))            # separate id space
        self.assertEqual(self.boot(bytes(garbage))["crash"]["first_seen_boot"], 1)
        garbage[500] = 0x22                                          # different garbage, new identity
        two = self.boot(bytes(garbage))["crash"]
        self.assertNotEqual(two["id"], one["id"])
        self.assertEqual(two["first_seen_boot"], 3)
        tiny = bytearray(b"\xff" * self.PART)
        tiny[0:4] = struct.pack("<I", 6)                             # too small to hold a checksum
        self.assertEqual(self.boot(bytes(tiny))["crash"]["error"], "invalid_size")

    def test_no_partition(self):
        self.assertEqual(self.boot(None)["crash"], {"present": False, "error": "no_partition"})


if __name__ == "__main__":
    unittest.main()

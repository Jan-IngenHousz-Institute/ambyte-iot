"""Contract group I: runtime integration of the REAL device_commands.c + sync_runner.c
with the two-media event store (event_log.c / evq_index.c / evq_render.c).

The C harness (tests/evq_integ/) runs the production drain, keeper, watchdog and
publisher tasks on a pthread-backed FreeRTOS shim with a virtual clock; this
module builds it under ASan+UBSan+LSan, runs every row three times, requires
byte-identical evidence across the runs, and re-checks the delivery manifests
with an independent Python oracle (sha256 of the spliced payload bytes).

Run:  CC=clang python -m unittest tests.test_evq_integration -v
Env:  EVQ_OUT (evidence root; writes $EVQ_OUT/integ/<row>/...), EVQ_TMPDIR
      (scratch root; default tempfile.mkdtemp(prefix="evq-")).
"""
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests" / "evq_integ"
REPEATS = 3
SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:",
                     "SUMMARY: AddressSanitizer", "SUMMARY: UndefinedBehaviorSanitizer")

# Tuning constants passed to EVERY translation unit (production value unless noted).
COMMON_DEFINES = {
    "EVSTORE_MOUNT": '"./evstore"',
    "SD_MOUNT_POINT": '"./sdcard"',
    "EVLOG_ROTATE_BYTES": "32768",          # production 256 KiB (smaller store for speed)
    "EVLOG_MIN_FREE_BYTES": "65536",        # production 256 KiB (scaled to the 1 MiB store)
    "EVLOG_EVICT_TARGET": "131072",         # production 512 KiB
    "EVLOG_ARCHIVE_EVERY_N": "250",         # production 1000
    "EVQ_PRESSURE_PCT": "25",               # production
    "EVQ_RECLAIM_PCT": "40",                # production
    "EVQ_SD_RESERVE_BYTES": "1048576ULL",   # production 64 MiB (scaled to the 256 MiB card)
    "EVQ_KEEPER_PERIOD_MS": "60000",        # production (I8 relies on it)
    "EVQ_INDEX_CAP": "4096",                # production
    "EVQ_INDEX_COMPACT_BYTES": "8192",      # production 48 KiB (exercise compaction)
}
VARIANTS = {
    "main": {},
    "idxcap": {"EVQ_INDEX_CAP": "12"},                 # flash_full_index_cap state
    "q1": {"AMBYTE_PUBLISH_MAX_BYTES": "16384"},       # documented conservative field build
}
# Production TUs build with -Werror too; nothing is suppressed for them. Only
# the third-party ESP-IDF cJSON.c is built with -w.
PRODUCTION_WNO = ["-Werror"]

PRODUCTION_SOURCES = [
    "components/event_log/event_log.c",
    "components/event_log/evq_index.c",
    "components/event_log/evq_render.c",
    "components/device_commands/device_commands.c",
    "components/device_commands/envelope_provenance.c",
    "components/device_commands/telemetry_publish.c",
    "components/payload_codec/payload_v3.c",
    "components/payload_codec/payload_scalar.c",
    "components/payload_codec/payload_gzip.c",
    "components/ambit_announcement/ambit_announcement.c",
]
HARNESS_SOURCES = ["tests/evq_integ/harness.c"]
IMPL_SOURCES = ["tests/evq_integ/rtos_shim.c", "tests/evq_integ/esp_stubs.c"]
INCLUDES = [
    "tests/evq_integ/stubs", "tests/evq_integ",
    "components/event_log/include", "components/event_log",
    "components/device_commands/include", "components/payload_codec/include",
    "components/domain/include", "components/sync_runner/include", "components/sync_runner",
    "components/sd_card", "components/clock_trust/include", "components/fleet_jitter/include",
    "components/timezone/include", "components/ota_update/include",
    "components/wifi_manager/include", "components/ambit_announcement/include",
]

STATE_SCENARIOS = [
    ("st_normal", "main"), ("st_blocked_sd_unavailable", "main"), ("st_blocked_sd_full", "main"),
    ("st_blocked_sd_error", "main"), ("st_blocked_sd_mismatch", "main"), ("st_sd_mismatch_head", "main"),
    ("st_blocked_backlog_waiting", "main"), ("st_blocked_index_cap", "idxcap"),
    ("st_blocked_transfer_pending", "main"), ("st_sd_lost_parked", "main"),
    ("st_backlog_corrupt", "main"), ("st_corrupt_flash", "main"), ("st_refused", "main"),
    ("st_unavailable", "main"), ("st_quarantined", "main"), ("st_upgrade", "main"),
    ("worst", "main"),
]

EVQ_INPUT_FIELDS = [
    "pending", "pending_exact", "storage_blocked", "deliverable_pending", "flash_pending", "sd_pending",
    "reimport_pending", "sd_state", "head_block", "blocked_reason", "refused_full", "refused_media",
    "refused_too_large", "refused_unavailable", "quarantined_poison", "quarantined_malformed",
    "skipped_unindexed_gap", "corrupt_detected", "corrupt_medium", "spool_files", "spool_errors",
    "mirror_used", "reclaimed_files", "archived_files", "reimported_files",
]
BOOL_FIELDS = {"available", "write_full", "pending_exact", "storage_blocked"}


def _cc():
    cc = os.environ.get("CC") or shutil.which("clang")
    if not cc or not shutil.which(cc):
        raise unittest.SkipTest("clang not available (set CC=clang)")
    return cc


def _cjson_dir():
    packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio"))) / "packages"
    found = sorted(packages.glob("framework-espidf*/components/json/cJSON/cJSON.c"))
    if not found:
        raise unittest.SkipTest("ESP-IDF cJSON source not found under ~/.platformio/packages")
    return found[0].parent


def parse_output(text):
    """KEY=VALUE lines → dict; repeated keys become lists."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"^([A-Z][A-Z0-9_]*)=(.*)$", line)
        if not m:
            continue
        k, v = m.group(1), m.group(2)
        if k in out:
            if not isinstance(out[k], list) or k not in ("I6_FILE", "I6_INDEX", "I8_TRACE", "SNAPSHOT_JSON",
                                                         "WORST_CLI_JSON", "WORST_TELEMETRY_JSON", "CHECK_FAIL"):
                out[k] = [out[k]] if not isinstance(out[k], list) else out[k]
            out[k].append(v)
        else:
            out[k] = [v] if k in ("I6_FILE", "I6_INDEX", "I8_TRACE", "SNAPSHOT_JSON", "WORST_CLI_JSON",
                                  "WORST_TELEMETRY_JSON", "CHECK_FAIL") else v
    return out


class EvqIntegration(unittest.TestCase):
    runs = {}          # (variant, scenario, seed) → list of results
    t_build = 0.0

    @classmethod
    def setUpClass(cls):
        cls.cc = _cc()
        cls.cjson = _cjson_dir()
        base = os.environ.get("EVQ_TMPDIR")
        if base:
            Path(base).mkdir(parents=True, exist_ok=True)
            cls.scratch = Path(tempfile.mkdtemp(prefix="evq-integ-", dir=base))
        else:
            cls.scratch = Path(tempfile.mkdtemp(prefix="evq-"))
        cls.out_root = Path(os.environ["EVQ_OUT"]) / "integ" if os.environ.get("EVQ_OUT") else None
        cls.failed = False
        t0 = time.monotonic()
        cls.binaries = {v: cls._build(v) for v in VARIANTS}
        cls.t_build = time.monotonic() - t0
        if cls.out_root:
            cls.out_root.mkdir(parents=True, exist_ok=True)
            (cls.out_root / "build.txt").write_text(cls.build_log)

    @classmethod
    def tearDownClass(cls):
        if cls.failed:
            print(f"\n[evq-integ] scratch kept for inspection: {cls.scratch}")
        else:
            shutil.rmtree(cls.scratch, ignore_errors=True)

    build_log = ""

    @classmethod
    def _build(cls, variant):
        bdir = cls.scratch / f"build-{variant}"
        bdir.mkdir(parents=True, exist_ok=True)
        defines = dict(COMMON_DEFINES)
        defines.update(VARIANTS[variant])
        dflags = [f"-D{k}={v}" for k, v in defines.items()]
        inc = [f"-I{ROOT / d}" for d in INCLUDES] + [f"-I{cls.cjson}"]
        base = ["-std=gnu11", "-D_GNU_SOURCE", "-g", "-O1", "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Wall", "-Wextra"]
        prelude = ["-include", str(HARNESS / "prelude.h")]
        jobs = []
        for src in PRODUCTION_SOURCES:
            extra = ["-Dpayload_v3_build_telemetry=h_capture_build_telemetry"] if src.endswith("device_commands.c") else []
            jobs.append((src, base + PRODUCTION_WNO + prelude + dflags + extra + inc))
        for src in HARNESS_SOURCES:
            jobs.append((src, base + ["-Werror"] + prelude + dflags + inc))
        for src in IMPL_SOURCES:
            jobs.append((src, base + ["-Werror", "-DEVQ_INTEG_IMPL_TU"] + dflags + inc))
        jobs.append((str(cls.cjson / "cJSON.c"), base + ["-w"] + inc))
        objs = []

        def compile_one(job):
            src, flags = job
            path = Path(src) if Path(src).is_absolute() else ROOT / src
            obj = bdir / (path.stem + ".o")
            cmd = [cls.cc, *flags, "-c", str(path), "-o", str(obj)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            return src, obj, cmd, r

        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            results = list(ex.map(compile_one, jobs))
        log = []
        for src, obj, cmd, r in results:
            log.append(" ".join(cmd))
            if r.stderr:
                log.append(r.stderr)
            if r.returncode != 0:
                cls.failed = True
                raise AssertionError(f"compile failed: {src}\n{r.stderr}")
            objs.append(str(obj))
        binary = bdir / "evq_integ"
        link = [cls.cc, "-fsanitize=address,undefined", *objs, "-lpthread", "-lm", "-o", str(binary)]
        r = subprocess.run(link, capture_output=True, text=True)
        log.append(" ".join(link))
        if r.returncode != 0:
            cls.failed = True
            raise AssertionError(f"link failed:\n{r.stderr}")
        cls.build_log += f"### variant {variant}\n" + "\n".join(log) + "\n"
        return binary

    # ── running ──

    def run_row(self, row, scenario, variant="main", seed=7, timeout=900):
        key = (variant, scenario, seed)
        if key in self.runs:
            return self.runs[key]
        results = []
        for k in range(REPEATS):
            rdir = Path(tempfile.mkdtemp(prefix=f"{scenario}-s{seed}-r{k}-", dir=self.scratch))
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:allocator_may_return_null=0"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            t0 = time.monotonic()
            p = subprocess.run([str(self.binaries[variant]), scenario, str(seed)], cwd=rdir,
                               capture_output=True, text=True, timeout=timeout, env=env)
            dt = time.monotonic() - t0
            res = {"rc": p.returncode, "stdout": p.stdout, "stderr": p.stderr, "dir": rdir,
                   "kv": parse_output(p.stdout), "seconds": dt}
            if self.out_root:
                od = self.out_root / row / f"{scenario}-seed{seed}" / f"run{k + 1}"
                od.mkdir(parents=True, exist_ok=True)
                (od / "stdout.txt").write_text(p.stdout)
                (od / "stderr.txt").write_text(p.stderr)
                for name in ("esp.log", "accepted.jsonl", "published.jsonl"):
                    if (rdir / name).exists():
                        shutil.copy(rdir / name, od / name)
                (od / "host_seconds.txt").write_text(f"{dt:.2f}\n")
            results.append(res)
        self.runs[key] = results
        return results

    def assert_clean_and_repeatable(self, results, label):
        for i, r in enumerate(results):
            for marker in SANITIZER_MARKERS:
                if marker in r["stderr"]:
                    type(self).failed = True
                    self.fail(f"{label} run {i + 1}: sanitizer report:\n{r['stderr'][-6000:]}")
            if r["rc"] != 0:
                type(self).failed = True
                fails = r["kv"].get("CHECK_FAIL", [])
                self.fail(f"{label} run {i + 1}: exit {r['rc']} (scratch {r['dir']})\n"
                          f"CHECK_FAIL: {fails}\nstderr tail:\n{r['stderr'][-4000:]}")
        first = results[0]["stdout"]
        for i, r in enumerate(results[1:], start=2):
            if r["stdout"] != first:
                type(self).failed = True
                a, b = first.splitlines(), r["stdout"].splitlines()
                diff = [f"run1: {x}\nrun{i}: {y}" for x, y in zip(a, b) if x != y][:5]
                self.fail(f"{label}: evidence differs between run 1 and run {i}:\n" + "\n".join(diff))
        if self.out_root:
            d = self.out_root / label
            d.mkdir(parents=True, exist_ok=True)
            (d / "repeatability.txt").write_text(
                f"runs={len(results)} identical_stdout=true sha256={hashlib.sha256(first.encode()).hexdigest()}\n")

    def j(self, results, key):
        return json.loads(results[0]["kv"][key])

    # ── independent delivery oracle (reads the manifests of run 1) ──

    def oracle(self, rdir, allowed_foreign=()):
        accepted = [json.loads(l) for l in (rdir / "accepted.jsonl").read_text().splitlines()]
        payloads = (rdir / "accepted_payloads.txt").read_text().split("\n")[:len(accepted)]
        pubs = [json.loads(l) for l in (rdir / "published.jsonl").read_text().splitlines()]
        envs = (rdir / "envelopes.txt").read_text().split("\n")[:len(pubs)]
        by_id = {a["id"]: (a, p) for a, p in zip(accepted, payloads)}
        first_seen, delivered, problems = {}, set(), []
        max_slots, bytes_violation = 0, 0
        for pub, env in zip(pubs, envs):
            if pub["msg_id"] == 0:
                continue
            doc = json.loads(env)                      # every envelope is valid JSON
            max_slots = max(max_slots, pub["out_count"])
            if pub["out_bytes"] > 65536 and pub["out_count"] > 1:
                bytes_violation += 1
            mid = pub["measure_id"]
            if mid not in by_id:
                if mid not in allowed_foreign:
                    problems.append(f"foreign id {mid}")
                continue
            a, payload = by_id[mid]
            if a["v3"]:
                end = env.rindex('],"timestamp":"')
                splice = env[len('{"sample":['):end]
            else:
                start = env.index(',"data":', env.index('"metadata":')) + len(',"data":')
                end = env.rindex('}],"timestamp":"')
                splice = env[start:end]
                s = doc["sample"][0]
                if (s["measure_id"], s["startTicks_UTC"], s["endTicks_UTC"], s["cmd_raw"]) != \
                        (a["id"], a["start_ms"], a["end_ms"], a["cmd_raw"]):
                    problems.append(f"v2 fields differ for {mid}")
                if s["metadata"] != (json.loads(a["metadata"]) if a["metadata"] else None):
                    problems.append(f"metadata differs for {mid}")
            if hashlib.sha256(splice.encode()).hexdigest() != hashlib.sha256(payload.encode()).hexdigest():
                problems.append(f"payload sha256 differs for {mid}")
            want_ts = datetime.datetime.fromtimestamp(a["start_ms"] // 1000, datetime.timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ")
            if doc["timestamp"] != want_ts:
                problems.append(f"timestamp {doc['timestamp']} != capture {want_ts} for {mid}")
            first_seen.setdefault(mid, pub["n"])
            if pub["outcome"] == 1:
                delivered.add(mid)
        missing = [a["id"] for a in accepted if a["id"] not in delivered]
        order = [first_seen[a["id"]] for a in accepted if a["id"] in first_seen]
        in_order = order == sorted(order) and len(order) == len(accepted)
        return {"accepted": len(accepted), "delivered": len(delivered), "missing": missing[:10],
                "missing_n": len(missing), "problems": problems[:10], "problems_n": len(problems),
                "first_delivery_in_order": in_order, "max_slots": max_slots,
                "byte_window_violations": bytes_violation}

    def assert_oracle(self, results, label, **kw):
        o = self.oracle(results[0]["dir"], **kw)
        if self.out_root:
            (self.out_root / label).mkdir(parents=True, exist_ok=True)
            (self.out_root / label / "python_oracle.json").write_text(json.dumps(o, indent=1) + "\n")
        self.assertEqual(o["missing_n"], 0, o)
        self.assertEqual(o["problems_n"], 0, o)
        self.assertTrue(o["first_delivery_in_order"], o)
        self.assertLessEqual(o["max_slots"], 16, o)
        self.assertEqual(o["byte_window_violations"], 0, o)
        self.assertGreater(o["accepted"], 0)
        return o

    # ── rows ──

    def test_I1_direct_drain_sd_only_and_flash(self):
        for seed in (1, 7, 1337):
            with self.subTest(seed=seed):
                res = self.run_row("I1", "I1", seed=seed)
                self.assert_clean_and_repeatable(res, f"I1-seed{seed}")
                kv = res[0]["kv"]
                pre = json.loads(kv["PRE_DRAIN_HEALTH"])
                self.assertGreater(pre["sd_pending"], 0)
                self.assertGreater(pre["flash_pending"], 0)
                e2e = json.loads(kv["I1_E2E"])
                self.assertEqual((e2e["missing"], e2e["mismatched"], e2e["timestamp_mismatch"], e2e["duplicates"]),
                                 (0, 0, 0, 0))
                self.assertTrue(e2e["first_delivery_order_ok"])
                broker = json.loads(kv["BROKER"])
                self.assertLessEqual(broker["max_outstanding_slots"], 16)
                self.assertEqual(broker["window_violations"], 0)
                self.assertEqual(json.loads(kv["DIRECT_CALLS"])["sync_runner_drain"] > 0, True)
                self.assert_oracle(res, f"I1-seed{seed}")

    def test_I2_refused_puback_hold(self):
        res = self.run_row("I2", "I2")
        self.assert_clean_and_repeatable(res, "I2")
        ref = self.j(res, "I2_REFUSAL")
        self.assertGreater(ref["refused_publishes"], 0)
        self.assertGreater(ref["max_hold_ms"], 0)
        self.assertEqual(ref["claims_while_held"], 0)
        self.assertFalse(ref["cursor_moved_before_clean_ack"])
        self.assertFalse(ref["pending_changed_before_clean_ack"])
        self.assertGreaterEqual(ref["first_clean_ack_after_connect_ms"], ref["refuse_window_ms"])
        self.assertEqual(int(res[0]["kv"]["I2_REFUSED_NEVER_REACKED"]), 0)
        self.assert_oracle(res, "I2")

    def test_I3_disconnect_full_window(self):
        res = self.run_row("I3", "I3")
        self.assert_clean_and_repeatable(res, "I3")
        w = self.j(res, "I3_WINDOW_AT_DISCONNECT")
        self.assertTrue(w["full"])
        after = self.j(res, "I3_AFTER_DISCONNECT")
        self.assertEqual(after["dc_slots"], 0)
        self.assertTrue(after["cursor_unchanged"] and after["pending_unchanged"])
        red = self.j(res, "I3_REDELIVERY")
        self.assertEqual(red["redelivered_ok"], red["unacked_at_disconnect"])
        self.assertTrue(red["reverted_first_fifo"])
        self.assert_oracle(res, "I3")

    def test_I4_head_waits_on_unreadable_sd(self):
        res = self.run_row("I4", "I4")
        self.assert_clean_and_repeatable(res, "I4")
        w = self.j(res, "I4_WINDOW")
        self.assertEqual(w["simulated_ms"], 600000)
        self.assertEqual(w["claims_not_found"], 0)
        self.assertEqual(w["claims_not_finished"], w["claims"])
        self.assertLessEqual(w["claims"], w["drain_wakes_notify"] + w["drain_wakes_fallback"] + 1)
        self.assertEqual((w["sync_runner_wait_logs"], w["event_log_wait_logs"]), (0, 0))
        ep = self.j(res, "I4_EPISODE")
        self.assertEqual((ep["sync_runner_wait_logs"], ep["event_log_wait_logs"]), (1, 1))
        self.assertLessEqual(ep["claims"], ep["drain_wakes"] + 1)
        self.assertGreater(ep["simulated_ms"], 3600000)
        self.assertEqual(w["publishes"], 0)
        d = self.j(res, "I4_DIRECT_PUBLISH")
        self.assertEqual(d["status"], "ESP_ERR_NOT_FINISHED")
        self.assertNotIn("no pending", d["message"])
        h = self.j(res, "I4_WAITING_HEALTH")
        self.assertGreater(h["pending"], 0)
        self.assertEqual(h["deliverable_pending"], 0)
        wd = self.j(res, "I4_WATCHDOG")
        self.assertFalse(wd["should_reboot"])
        self.assertTrue(wd["power_ok"] and wd["clock_ok"])
        self.assertGreater(wd["since_ms"], wd["timeout_ms"])
        self.assert_oracle(res, "I4")

    def _snapshots(self):
        snaps, worst_cli, worst_tel, worst_emit = {}, [], [], None
        for scenario, variant in STATE_SCENARIOS:
            res = self.run_row("I5_I9_states", scenario, variant=variant)
            self.assert_clean_and_repeatable(res, f"states-{scenario}")
            for raw in res[0]["kv"].get("SNAPSHOT_JSON", []):
                s = json.loads(raw)
                snaps[s["name"]] = s
            worst_cli += [json.loads(x) for x in res[0]["kv"].get("WORST_CLI_JSON", [])]
            worst_tel += [json.loads(x) for x in res[0]["kv"].get("WORST_TELEMETRY_JSON", [])]
            if "WORST_EMIT_JSON" in res[0]["kv"]:
                worst_emit = json.loads(res[0]["kv"]["WORST_EMIT_JSON"])
        return snaps, worst_cli, worst_tel, worst_emit

    EXPECT = {
        "normal": {"storage_blocked": False, "blocked_reason": "none", "sd_state": "ok"},
        "blocked_sd_unavailable": {"storage_blocked": True, "blocked_reason": "flash_full_sd_unavailable", "sd_state": "absent"},
        "blocked_sd_full": {"storage_blocked": True, "blocked_reason": "flash_full_sd_full", "sd_state": "full"},
        "blocked_sd_error": {"storage_blocked": True, "blocked_reason": "flash_full_sd_error"},
        "sd_mismatch": {"sd_state": "mismatch", "head_block": "sd_mismatch", "deliverable_pending": 0},
        "sd_mismatch_reimport": {"sd_state": "mismatch", "head_block": "none"},
        "blocked_sd_mismatch": {"storage_blocked": True, "blocked_reason": "flash_full_sd_mismatch", "sd_state": "mismatch"},
        "blocked_backlog_waiting": {"storage_blocked": True, "blocked_reason": "flash_full_backlog_waiting"},
        "blocked_index_cap": {"storage_blocked": True, "blocked_reason": "flash_full_index_cap"},
        "blocked_transfer_pending": {"storage_blocked": True, "blocked_reason": "flash_full_transfer_pending"},
        "sd_lost": {"sd_state": "lost"},
        "sd_parked": {"sd_state": "parked"},
        "sd_backlog_corrupt": {"sd_state": "backlog_corrupt", "corrupt_medium": "sd"},
        "corrupt_flash": {"corrupt_medium": "flash", "sd_state": "backlog_corrupt"},
        "pending_inexact": {"pending_exact": False},
    }

    def test_I5_status_heartbeat_every_state(self):
        snaps, _, worst_tel, worst_emit = self._snapshots()
        for name, want in self.EXPECT.items():
            with self.subTest(state=name):
                self.assertIn(name, snaps)
                h = snaps[name]["health"]
                for k, v in want.items():
                    got = h[k]
                    if k in BOOL_FIELDS:
                        got = bool(got)
                    self.assertEqual(got, v, f"{name}.{k}")
        covered = lambda f: any(s["health"][f] > 0 for s in snaps.values())
        for f in ("refused_full", "refused_media", "refused_too_large", "refused_unavailable",
                  "quarantined_poison", "quarantined_malformed", "corrupt_detected", "skipped_unindexed_gap"):
            with self.subTest(nonzero=f):
                self.assertTrue(covered(f), f"no state with non-zero {f}")
        reasons = {s["health"]["blocked_reason"] for s in snaps.values()}
        self.assertTrue({"flash_full_sd_unavailable", "flash_full_sd_full", "flash_full_sd_error",
                         "flash_full_sd_mismatch", "flash_full_backlog_waiting", "flash_full_index_cap",
                         "flash_full_transfer_pending"} <= reasons, reasons)
        states = {s["health"]["sd_state"] for s in snaps.values()}
        self.assertTrue({"absent", "lost", "parked", "mismatch", "full", "backlog_corrupt"} <= states, states)
        for name, s in snaps.items():
            with self.subTest(snapshot=name):
                self.assertEqual(s["emit_status"], "ESP_OK")
                self.assertTrue(s["stable"])
                env = s["envelope"]
                self.assertIsNotNone(env)
                sample = env["sample"][0]
                self.assertEqual(sample["schema"], "ambyte.telemetry/1")
                evq = sample["health"]["storage"]["evq"]
                h = s["health"]
                for f in EVQ_INPUT_FIELDS:
                    want = bool(h[f]) if f in BOOL_FIELDS else h[f]
                    self.assertEqual(evq[f], want, f"{name}: STATUS evq.{f}")
                    if s.get("capture"):
                        self.assertEqual(s["capture"][f], want, f"{name}: builder input {f}")
        self.assertEqual(len(worst_tel), 2)
        for w in worst_tel:
            with self.subTest(worst=w["variant"]):
                self.assertEqual(w["cap"], 6144)
                if w["fits"]:
                    self.assertLess(w["worst_len"], w["cap"])
                    self.assertEqual(w["payload"]["schema"], "ambyte.telemetry/1")
                else:
                    self.assertIsNone(w["payload"])
                    self.assertGreaterEqual(w["worst_len"], w["cap"])
                    self.assertIn("exceeds buffer", w["error"])
        self.assertIsNotNone(worst_emit)
        if worst_emit["status"] == "ESP_OK":
            self.assertTrue(worst_emit["published"])
            self.assertEqual(worst_emit["envelope"]["sample"][0]["schema"], "ambyte.telemetry/1")
        else:
            self.assertEqual(worst_emit["status"], "ESP_ERR_INVALID_SIZE")
            self.assertFalse(worst_emit["published"])
        if self.out_root:
            (self.out_root / "I5").mkdir(parents=True, exist_ok=True)
            (self.out_root / "I5" / "worst_case.txt").write_text(
                "".join(f"PAYLOAD_V3_TELEMETRY_CAP={w['cap']} variant={w['variant']} worst_len={w['worst_len']} "
                        f"fits={w['fits']}\n" for w in worst_tel) +
                f"real_emit_adversarial_ports status={worst_emit['status']} published={worst_emit['published']}\n")

    CLI_KEYS = {
        "available": "available", "pending_exact": "pending_exact", "deliverable_pending": "deliverable_pending",
        "flash_pending": "flash_pending", "sd_pending": "sd_pending", "reimport_pending": "reimport_pending",
        "next_id": "next_id", "last_acked_id": "last_acked_id", "sd_state": "sd_state", "head_block": "head_block",
        "storage_blocked": "storage_blocked", "blocked_reason": "blocked_reason", "write_full": "write_full",
        "refused_full": "refused_full", "refused_media": "refused_media", "refused_too_large": "refused_too_large",
        "refused_unavailable": "refused_unavailable", "dropped": "dropped",
        "quarantined_poison": "quarantined_poison", "quarantined_malformed": "quarantined_malformed",
        "skipped_unindexed_gap": "skipped_unindexed_gap", "corrupt_detected": "corrupt_detected",
        "corrupt_medium": "corrupt_medium", "skipped": "skipped", "spool_files": "spool_files",
        "spool_errors": "spool_errors", "mirror_used": "mirror_used", "reclaimed_files": "reclaimed",
        "archived_files": "archived", "reimported_files": "reimported", "pressure_notifies": "pressure_notifies",
        "sd_bursts": "sd_bursts",
    }

    def check_cli(self, text, h, label):
        tokens = dict(re.findall(r"([a-z_]+)=([^\s]+)", text))
        for field, tok in self.CLI_KEYS.items():
            want = h[field]
            if field in BOOL_FIELDS:
                want = 1 if want else 0
            self.assertIn(tok, tokens, f"{label}: token {tok}")
            self.assertEqual(str(want), tokens[tok], f"{label}: {tok}")
        if h["pending_exact"]:
            self.assertIn(f"pending={h['pending']} ", text, label)
        else:
            self.assertIn(f"pending>={h['pending']} ", text, label)
        self.assertEqual(tokens["cursor"], f"ev-{h['rd_seq']:06d}", label)
        self.assertEqual(tokens["tail"], f"ev-{h['tail_seq']:06d}", label)
        self.assertEqual(tokens["index"], f"{h['index_segments']}/{h['index_cap']}", label)
        self.assertTrue(text.endswith("\r\n"), f"{label}: rendering not line-complete")

    def test_I9_evlog_cli_every_state(self):
        snaps, worst_cli, _, _ = self._snapshots()
        for name, s in snaps.items():
            if s["cli_len"] == -2:
                continue           # stored-heartbeat record: no CLI call in that snapshot
            with self.subTest(state=name):
                self.assertEqual(s["cli_len"], len(s["cli"].encode()))
                self.check_cli(s["cli"], s["health"], name)
        self.assertTrue(any(not s["health"]["pending_exact"] for s in snaps.values()))
        self.assertEqual(len(worst_cli), 2)
        for w in worst_cli:
            with self.subTest(worst=w["counters"]):
                # The worst case must FIT the production CLI buffer...
                self.assertEqual(w["cap"], 1536)
                self.assertGreater(w["rc_at_cap"], 0)
                self.assertEqual(w["rc_at_cap"], w["true_len"])
                self.assertLess(w["true_len"], 1536)
                self.check_cli(w["text"], w["health"], f"worst-{w['counters']}")
                self.check_cli(w["full_text"], w["health"], f"worst-full-{w['counters']}")
                # ...and a deliberately small buffer refuses instead of truncating.
                self.assertEqual(w["rc_small"], -1)
        cli = (ROOT / "components/CLI/CLI.c").read_text()
        body = cli[cli.index("static int cli_cmd_evlog("):]
        body = body[:body.index("\n}\n")]
        self.assertRegex(body, r"static char text\[1536\];")
        self.assertRegex(body, r"if \(evq_render_health_text\(&h, text, sizeof text\) < 0\) \{\s*printf\(\"evlog: status "
                               r"rendering exceeded %u B \u2014 refusing to print a truncated status")
        self.assertIn('printf("%s", text);', body)
        if self.out_root:
            (self.out_root / "I9").mkdir(parents=True, exist_ok=True)
            (self.out_root / "I9" / "worst_case.txt").write_text(
                "".join(f"CLI_BUFFER=1536 counters={w['counters']} rc_at_cap={w['rc_at_cap']} "
                        f"true_len={w['true_len']} small_cap={w['small_cap']} rc_small={w['rc_small']}\n"
                        for w in worst_cli))

    def test_I6_keeper_task_matches_direct_run(self):
        a = self.run_row("I6", "I6_task")
        b = self.run_row("I6", "I6_direct")
        self.assert_clean_and_repeatable(a, "I6-task")
        self.assert_clean_and_repeatable(b, "I6-direct")
        ka, kb = a[0]["kv"], b[0]["kv"]
        self.assertEqual(ka["I6_COUNTS"], kb["I6_COUNTS"])
        self.assertEqual(ka.get("I6_INDEX"), kb.get("I6_INDEX"))
        self.assertEqual(ka.get("I6_FILE"), kb.get("I6_FILE"))
        self.assertEqual(json.loads(ka["DIRECT_CALLS"])["event_log_sd_service"], 0)
        self.assertGreater(json.loads(kb["DIRECT_CALLS"])["event_log_sd_service"], 0)
        counts = json.loads(ka["I6_COUNTS"])
        self.assertGreater(counts["spool_files"], 0)
        self.assertGreater(counts["reclaimed_files"], 0)
        if self.out_root:
            (self.out_root / "I6" / "comparison.txt").write_text(
                f"task={ka['I6_COUNTS']}\ndirect={kb['I6_COUNTS']}\nindex_equal=True files_equal=True\n")

    def test_I7_automatic_delivery_production_tasks(self):
        res = self.run_row("I7", "I7")
        self.assert_clean_and_repeatable(res, "I7")
        t = self.j(res, "I7_TRACE")
        dc = self.j(res, "DIRECT_CALLS")
        self.assertEqual((dc["sync_runner_drain"], dc["event_log_sd_service"]), (0, 0))
        self.assertGreater(t["drain_passes"], 0)
        self.assertEqual(t["drain_passes_outside_sync_runner"], 0)
        self.assertEqual(t["drain_passes_without_wake"], 0)
        self.assertGreater(t["wakes_notify"], 0)
        self.assertGreater(t["wakes_fallback"], 0)
        self.assertEqual(t["claims_while_gate_closed"], 0)
        self.assertEqual(t["claims_during_sensor_hold"], 0)
        self.assertGreater(t["claims_after_reopen"], 0)
        self.assertGreaterEqual(t["gate_opens"], 2)
        self.assertGreaterEqual(t["gate_closes"], 1)
        self.assert_oracle(res, "I7")

    def test_I8_keeper_wakes_on_pressure_mount_unpark(self):
        res = self.run_row("I8", "I8")
        self.assert_clean_and_repeatable(res, "I8")
        p = self.j(res, "I8_PRESSURE")
        self.assertEqual(p["keeper_period_ms"], 60000)
        self.assertEqual(p["t_notify_ms"], p["t_store_ms"])
        self.assertEqual(p["t_keeper_wake_ms"], p["t_store_ms"])
        self.assertEqual(p["t_first_sd_create_ms"], p["t_store_ms"])
        self.assertLess(p["t_keeper_wake_ms"], p["next_periodic_ms"])
        self.assertGreater(p["spool_files_after"], 0)
        m = self.j(res, "I8_MOUNT_UNPARK")
        self.assertEqual(m["t_mount_wake_ms"], m["t_mount_ms"])
        self.assertEqual(m["t_unpark_wake_ms"], m["t_unpark_ms"])
        trace = res[0]["kv"]["I8_TRACE"]
        at = [t.split() for t in trace if int(t.split()[0]) == p["t_store_ms"]]
        kinds = [(t[1], t[2]) for t in at]
        # store → notify → keeper wake → keeper SD transfer, all at the store's virtual instant
        seq = [("test", "store"), ("test", "notify"), ("sd_keeper", "wake"), ("sd_keeper", "sd_create")]
        pos = [kinds.index(k) for k in seq]
        self.assertEqual(pos, sorted(pos), kinds)
        self.assertEqual(self.j(res, "DIRECT_CALLS")["event_log_sd_service"], 0)

    def test_F1_reimport_obligation_not_evicted(self):
        res = self.run_row("F1_reimport_evict", "reimport_evict")
        self.assert_clean_and_repeatable(res, "F1_reimport_evict")
        r = self.j(res, "REIMPORT_EVICT")
        # the keeper wakes at boot, so obligations may already be re-imported at the first sample
        self.assertGreater(r["reimport_pending_after_boot"] + r["reimported_files_after_fill"], 0)
        self.assertFalse(r["dropped_without_reimport"], r)
        self.assertTrue(r["finished"], r)
        self.assertEqual((r["reimport_pending_end"], r["pending_end"]), (0, 0))
        self.assertGreater(r["reimported_files_end"], 0)
        e2e = self.j(res, "REIMPORT_E2E")
        self.assertEqual((e2e["missing"], e2e["mismatched"], e2e["timestamp_mismatch"]), (0, 0, 0))
        self.assertEqual(e2e["accepted"], e2e["delivered"])
        o = self.oracle(res[0]["dir"])
        self.assertEqual((o["missing_n"], o["problems_n"]), (0, 0), o)

    def test_F2_remount_reverifies_sd_copies(self):
        for scenario in ("epoch_primary", "epoch_both"):
            with self.subTest(variant=scenario):
                res = self.run_row("F2_epoch", scenario)
                self.assert_clean_and_repeatable(res, f"F2-{scenario}")
                r = self.j(res, "EPOCH_RESULT")
                self.assertGreater(r["altered_files"], 0)
                self.assertEqual(r["delivered_with_altered_bytes"], 0, r)
                after = self.j(res, "EPOCH_AFTER_HEALTH")
                if scenario == "epoch_primary":
                    self.assertGreater(r["mirror_used"], 0)
                    self.assertEqual(r["delivered"], r["accepted"])
                    self.assert_oracle(res, f"F2-{scenario}")
                else:
                    self.assertEqual(r["delivered"], 0)
                    self.assertEqual((after["sd_state"], after["corrupt_medium"]), ("backlog_corrupt", "sd"))
                    self.assertEqual(after["deliverable_pending"], 0)
                    o = self.oracle(res[0]["dir"])
                    self.assertEqual(o["problems_n"], 0, o)   # nothing altered was ever published

    def test_F4_index_cap_replay(self):
        res = self.run_row("F4_index_cap", "index_cap_replay", variant="idxcap")
        self.assert_clean_and_repeatable(res, "F4_index_cap")
        kv = res[0]["kv"]
        b1 = json.loads(kv["IDXCAP_BOOT1_HEALTH"])
        b2 = json.loads(kv["IDXCAP_BOOT2_HEALTH"])
        d1 = json.loads(kv["IDXCAP_DISK_BOOT1"])
        d_before = json.loads(kv["IDXCAP_DISK_BEFORE_REBOOT"])
        d_cap = json.loads(kv["IDXCAP_DISK_AT_CAP"])
        self.assertEqual(b1["index_cap"], 12)
        self.assertGreater(b1["tail_seq"], 3 * 12)                 # rotated well past the cap
        self.assertEqual(b1["index_segments"], 12)
        # Live RAM index == on-disk fold: no line names a segment RAM refused.
        self.assertEqual(d1["n"], b1["index_segments"], d1)
        self.assertEqual(d1["bad_lines"], 0)
        # Replay at the production cap rejects nothing and yields the same table.
        self.assertEqual(d_cap["bad_lines"], 0, d_cap)
        self.assertEqual(d_cap["segs"], d_before["segs"])
        self.assertEqual(b2["index_segments"], b1["index_segments"])
        self.assertEqual(d_before["segs"], d1["segs"])

    def test_J_sanitizer_report_fails_the_run(self):
        """A sanitizer report must fail a row: canaries with a heap overflow, a
        signed overflow and a leak must each exit non-zero with the report."""
        for canary, marker in (("canary_asan", "ERROR: AddressSanitizer"),
                               ("canary_ubsan", "runtime error:"),
                               ("canary_lsan", "ERROR: LeakSanitizer")):
            with self.subTest(canary=canary):
                rdir = Path(tempfile.mkdtemp(prefix=f"{canary}-", dir=self.scratch))
                env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
                p = subprocess.run([str(self.binaries["main"]), canary, "7"], cwd=rdir,
                                   capture_output=True, text=True, timeout=120, env=env)
                self.assertNotEqual(p.returncode, 0, canary)
                self.assertIn(marker, p.stderr, canary)
                shutil.rmtree(rdir, ignore_errors=True)

    def test_Q1_oversize_pre_upgrade_record(self):
        res = self.run_row("Q1_oversize", "Q1_oversize", variant="q1")
        self.assert_clean_and_repeatable(res, "Q1_oversize")
        q = self.j(res, "Q1_OVERSIZE")
        self.assertEqual(q["publish_cap"], 16384)
        self.assertTrue(q["quarantine_intact"])
        self.assertGreater(q["first_cursor_past_fixture_trace_idx"], q["fsync_quarantine_trace_idx"])
        self.assertEqual(q["quarantined_poison_after"], q["quarantined_poison_before"] + 1)
        self.assertEqual(q["oversize_skipped_from_message"], 1)
        self.assertFalse(q["fixture_published"])
        self.assertEqual(q["cursor_before"], "1:0")
        o = self.assert_oracle(res, "Q1_oversize")
        self.assertEqual(o["accepted"], 30)


if __name__ == "__main__":
    unittest.main()

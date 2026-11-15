# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Brownout protection (docs/power-protect.md).

Compiles the PRODUCTION state machine (components/power_protect/power_protect_core.c),
its glue (power_protect.c, against a fake NVS that counts every write), the real
power_policy gate that supplies the fresh-external-power evidence, and the MQTT
command codec (components/command_router/power_protect_command.c, pinned cJSON)
into tests/power_protect_host and runs every scenario: every transition and exit
timer, window restarts on invalid / stale / no_input / out-of-threshold reads and
read gaps, battery_v == 0 with input, forced states, clear, brownout escalation
with a valid and an invalid clock (the 15 Nov Uluru boots 2-3-4), the blob
round-trip and NVS writes only on transitions, and the command codec.

Real brownout behaviour is covered here only: a bench cannot brown out on demand,
so the bench run exercises `force` and `clear`. Source-contract tests pin the
wiring the host build cannot link (boot order, shedding, refusal sites). Run with
CC=clang."""

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/power_protect_host"


def _cjson():
    from pinned_idf import pinned_idf
    return pinned_idf() / "components/json/cJSON"


def build(out: Path) -> Path:
    cc = os.environ.get("CC") or shutil.which("clang") or "cc"
    binary = out / "power_protect_host"
    cjson = _cjson()
    cmd = [cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           f"-I{HOST / 'include'}",
           f"-I{ROOT / 'tests/diag_host/include'}",            # esp_system.h (reset reasons)
           f"-I{ROOT / 'components/power_protect/include'}",
           f"-I{ROOT / 'components/domain/include'}",
           f"-I{ROOT / 'components/device_commands/include'}",
           f"-I{ROOT / 'components/command_router/include'}",
           f"-I{ROOT / 'components/clock_trust/include'}",
           f"-I{ROOT / 'components/diag/include'}",
           f"-I{cjson}",
           str(HOST / "power_protect_host.c"),
           str(ROOT / "components/power_protect/power_protect_core.c"),
           str(ROOT / "components/device_commands/power_policy.c"),
           str(ROOT / "components/command_router/power_protect_command.c"),
           str(cjson / "cJSON.c"),
           "-Wl,--wrap=time", "-lm", "-o", str(binary)]
    if "clang" in Path(cc).name:
        cmd[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return binary


class PowerProtectHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="power-protect-")
        try:
            cls.bin = build(Path(cls.tmp.name))
        except subprocess.CalledProcessError as e:
            raise AssertionError(e.stderr) from e
        cls.scenarios = subprocess.run([str(cls.bin), "--list"], check=True, capture_output=True,
                                       text=True).stdout.split()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, name):
        r = subprocess.run([str(self.bin), name], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"PASS {name}", r.stdout)
        return r.stdout

    def replies(self, name):
        return [json.loads(l[len("REPLY "):]) for l in self.run_case(name).splitlines()
                if l.startswith("REPLY ")]

    def test_every_scenario_passes(self):
        self.assertGreaterEqual(len(self.scenarios), 25)
        for name in self.scenarios:
            with self.subTest(scenario=name):
                self.run_case(name)

    def test_heartbeat_object(self):
        out = self.run_case("render_json")
        obj = json.loads(re.search(r"^JSON (.*)$", out, re.M).group(1))
        self.assertEqual(set(obj), {"state", "reason", "since_utc", "forced", "persist_fault", "brownouts_6h",
                                    "skipped"})
        self.assertIs(obj["persist_fault"], False)
        self.assertEqual(obj["skipped"], {"flash_runs": 2, "ambit_runs": 1, "ota": 1})
        worst = re.search(r"^WORST (\d+) (.*)$", out, re.M)
        self.assertLess(int(worst.group(1)), 256)
        self.assertEqual(json.loads(worst.group(2))["reason"], "brownout_unordered")   # the longest name
        unreadable = json.loads(re.search(r"^JSON_UNREADABLE (.*)$", self.run_case("unreadable_state_is_protected"),
                                          re.M).group(1))
        self.assertEqual((unreadable["state"], unreadable["reason"], unreadable["forced"], unreadable["persist_fault"]),
                         ("no_flash", "state_unreadable", False, True))

    def test_command_flow_replies(self):
        status, forced, cleared, status2 = self.replies("cmd_flow")
        self.assertEqual(status["type"], "power_protect_result")
        self.assertEqual((status["id"], status["ok"], status["action"], status["detail"]),
                         ("s1", True, "status", "ok"))
        self.assertEqual((status["state"], status["reason"], status["forced"], status["brownouts_6h"],
                          status["persist_fault"]), ("no_flash", "brownout", False, 1, False))
        self.assertIsInstance(status["since_utc"], (int, float))
        self.assertIsNone(status["exit_in_s"])
        self.assertEqual(status["skipped"], {"flash_runs": 0, "ambit_runs": 0, "ota": 0})
        self.assertEqual((forced["ok"], forced["detail"], forced["state"], forced["reason"], forced["forced"]),
                         (True, "applied", "hold", "forced", True))
        self.assertEqual((cleared["ok"], cleared["detail"], cleared["state"], cleared["reason"],
                          cleared["forced"], cleared["brownouts_6h"]),
                         (True, "cleared", "normal", "cleared", False, 0))
        self.assertTrue(status2["ok"])

    def test_command_refusals(self):
        got = [(r["id"], r["ok"], r["detail"]) for r in self.replies("cmd_refusals")]
        self.assertEqual(got[:12], [
            ("", False, "invalid_id"), ("", False, "invalid_id"), ("", False, "invalid_id"),
            ("", False, "invalid_id"), ('q"1', False, "invalid_action"), ("a2", False, "invalid_action"),
            ("a3", False, "invalid_action"), ("f2", False, "invalid_state"), ("f3", False, "invalid_state"),
            ("f4", False, "invalid_state"), ("c2", False, "invalid_state"), ("s3", False, "invalid_state"),
        ])
        self.assertEqual(got[12:], [("b2", False, "busy"), ("b1", True, "ok"),
                                    ("q1", False, "queue_unavailable"), ("q2", True, "ok"),
                                    ("m1", False, "maintenance_active")])
        for r in self.replies("cmd_refusals"):
            if not r["ok"]:
                self.assertEqual(r["state"], "normal")   # refusals still say where the unit is

    def test_persist_failure_is_reported(self):
        (r,) = self.replies("cmd_persist_failed")
        self.assertEqual((r["ok"], r["detail"], r["state"], r["forced"], r["persist_fault"]),
                         (False, "persist_failed", "no_flash", True, True))

    def test_allocation_failures_never_report_partial_success(self):
        replies = self.replies("cmd_no_mem")
        self.assertEqual(len(replies), 41)
        details = {r["detail"] for r in replies}
        self.assertLessEqual(details, {"queue_unavailable", "no_mem", "ok"})
        self.assertIn("no_mem", details)
        self.assertEqual(replies[0]["detail"], "queue_unavailable")
        for r in replies:
            if r["ok"]:
                self.assertEqual(r["detail"], "ok")
                self.assertIn("skipped", r)
                self.assertEqual(set(r["skipped"]), {"flash_runs", "ambit_runs", "ota"})
                self.assertIn("persist_fault", r)


class AmbitWorkerProtectionTest(unittest.TestCase):
    """Review of PR #103, AMBIT side. Round 1: re-check after the ack flush /
    fleet spread, after the download or release fetch, before each channel.
    Round 2: the admission must sit at the LAST safe point before the first
    destructive step, after every blocking probe, identity read, preflight,
    quiesce or flush: protection beginning INSIDE the quiesce, the suspend, a
    ping, the version read, the ROM probe, the SD preflight, the UART bus wait
    or the ROM connect must still stop the erase / OTA_BEGIN. The PRODUCTION
    worker functions and writers (ambit_ota_one_impl, ambit_flash_image_gated,
    ambit_flash_attach_flash) are extracted verbatim; a write that has started
    finishes, no later one starts, and a power refusal never consumes the
    attach/boot-sync fail cap. The Ambyte side runs in tests/test_ota_admission.py."""

    SCENARIOS = ("ota_protected_at_entry", "ota_flip_during_spread", "ota_flip_during_quiesce",
                 "ota_flip_during_download", "ota_flip_during_ping", "ota_flip_during_identity",
                 "ota_flip_mid_sweep", "flash_flip_during_ack", "flash_flip_during_suspend",
                 "flash_flip_during_fetch", "flash_flip_during_probe", "flash_flip_during_preflight",
                 "flash_flip_during_bus_wait", "flash_flip_during_rom_connect", "flash_flip_mid_sweep",
                 "attach_flip_during_rom_connect", "attach_unprotected", "ota_unprotected",
                 "flash_unprotected")

    @classmethod
    def setUpClass(cls):
        from test_schedule_light_cleanup import function
        ota = (ROOT / "components/ambit_ota/ambit_ota.c").read_text()
        flash = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        regions = flash[flash.index("typedef struct {\n    uint32_t    offset;"):]
        regions = regions[:regions.index("\n", regions.index("#define NUM_REGIONS")) + 1]
        cls.tmp = tempfile.TemporaryDirectory(prefix="ambit-worker-")
        tmp = Path(cls.tmp.name)
        (tmp / "ambit-worker-production.inc").write_text("\n".join(
            [regions,
             "static esp_loader_error_t flash_one_region(esp_loader_t *, const char *, const flash_region_t *,"
             " uint32_t *);"]
            + [function(flash, name) for name in ("boot_fail_key", "boot_fail_count", "boot_fail_bump",
                                                  "boot_fail_clear", "flash_power_allows_write",
                                                  "boot_fail_restore", "fw_matches", "ambit_flash_image_gated",
                                                  "ambit_flash_attach_flash")]
            + [function(ota, name) for name in ("power_note_refusal", "power_blocks_write", "power_allows_write",
                                                "ambit_ota_one_impl", "ambit_ota_one", "ambit_do_ota",
                                                "ambit_do_flash", "ambit_ota_run")]))
        cc = os.environ.get("CC") or shutil.which("clang") or "cc"
        cls.bin = tmp / "ambit_worker"
        cmd = [cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", f"-I{tmp}", str(HOST / "ambit_worker_driver.c"), "-o", str(cls.bin)]
        if "clang" in Path(cc).name:
            cmd[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def reports(self, name):
        r = subprocess.run([str(self.bin), name], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"PASS {name}", r.stdout)
        return [l.split()[1:] for l in r.stdout.splitlines() if l.startswith("REPORT ")]

    def test_every_scenario(self):
        for name in self.SCENARIOS:
            with self.subTest(scenario=name):
                self.reports(name)

    def test_refusals_are_terminal_power_protect(self):
        for name in self.SCENARIOS:
            if name.startswith("attach_") or name.endswith("_unprotected"):
                continue
            kind = "ambit_flash_status" if name.startswith("flash_") else "ambit_ota_status"
            with self.subTest(scenario=name):
                last = self.reports(name)[-1]
                self.assertEqual(last[:2], [kind, "failed"])
                self.assertEqual(last[-1], "detail=power_protect")
        # Entry refusal comes before any ack; later ones after it.
        self.assertEqual(len(self.reports("ota_protected_at_entry")), 1)
        self.assertEqual(self.reports("ota_flip_during_spread")[0][:2], ["ambit_ota_status", "accepted"])
        sweep = self.reports("flash_flip_mid_sweep")
        self.assertIn(["ambit_flash_status", "flash_ok", "ch=0", "id=job-1", "detail=-"], sweep)
        for ch in (1, 2, 3):
            self.assertIn(["ambit_flash_status", "skipped", f"ch={ch}", "id=job-1", "detail=power_protect"], sweep)
        # Round 2: protection during ch0's ROM probe / preflight / bus wait / ROM
        # connect: ch0 is skipped (never erased), not reported flash_ok.
        for name in ("flash_flip_during_probe", "flash_flip_during_preflight", "flash_flip_during_bus_wait",
                     "flash_flip_during_rom_connect"):
            with self.subTest(scenario=name):
                got = self.reports(name)
                self.assertIn(["ambit_flash_status", "skipped", "ch=0", "id=job-1", "detail=power_protect"], got)
                self.assertNotIn("flash_ok", [g[1] for g in got])
        for name in ("ota_flip_during_ping", "ota_flip_during_identity"):
            with self.subTest(scenario=name):
                got = self.reports(name)
                self.assertIn(["ambit_ota_status", "failed", "ch=0", "id=job-1", "detail=power_protect"], got)
                self.assertNotIn("success", [g[1] for g in got])


class PowerProtectWiringContractTest(unittest.TestCase):
    """What the host build cannot link: the production call sites."""

    def read(self, rel):
        return (ROOT / rel).read_text()

    def test_boot_evaluates_before_anything_that_sheds(self):
        src = self.read("main/app_main.c")
        main = src[src.index("void app_main(void)"):]
        boot = main.index("power_protect_boot();")
        self.assertLess(main.index("diag_boot_init();"), boot)
        self.assertLess(main.index("err = app_init_i2c_and_sensors();"), boot)   # clock trust decided
        self.assertLess(boot, main.index("I2C startup failed"))                  # both sides of the check
        self.assertLess(boot, main.index("ambit_flash_boot_sync()"))
        self.assertLess(boot, main.index("app_start_sched_runner();"))
        self.assertLess(boot, main.index("app_open_boot_gate();\n\n#if CONFIG_AMBYTE_BENCH_DIAG"))

    def test_reads_feed_protection_with_flash_evidence(self):
        src = self.read("components/device_commands/device_commands.c")
        body = src[src.index("static void power_state_refresh(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("power_protect_on_power_read(reason, reason == POWER_READ_OK ? &p : NULL,\n"
                      "                                power_gate_flash_ok(&s_pub_gate));", body)
        guard = self.read("main/app_main.c")
        task = guard[guard.index("static void app_power_guard_task(void *arg)"):]
        task = task[:task.index("\nstatic void app_start_power_guard")]
        self.assertIn("device_commands_power_tick();", task)

    def test_flash_gate_refuses_while_protected(self):
        src = self.read("components/device_commands/device_commands.c")
        body = src[src.index("bool device_commands_flash_power_ok(void)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("return AMBYTE_FLASH_WITHOUT_CHARGER != 0 && !power_protect_blocks_flash();", body)
        self.assertIn("if (power_protect_blocks_flash()) return false;", body)

    def test_runner_sheds_before_any_uart_and_never_counts_a_failure(self):
        src = self.read("components/sched_runner/sched_runner.c")
        run = src[src.index("static void run_job(int j)\n{"):]
        run = run[:run.index("\n}\n")]
        self.assertLess(run.index("if (shed_job(j, name)) return;"), run.index("sensor_inventory_run_begin();"))
        shed = src[src.index("static bool shed_job(int j, const char *name)"):]
        shed = shed[:shed.index("\n}\n")]
        self.assertIn("sched_job_account(&s_rt[j].n, SCHED_RUN_SHED)", shed)
        self.assertNotIn("failures", shed)
        self.assertNotIn("event_log", shed)
        self.assertNotIn("store_event", shed)

    def test_updates_refused_at_every_boundary(self):
        # Review of PR #103: one entry check is not enough; the worker re-checks
        # after the ack flush / fleet spread and before selecting the image.
        ota = self.read("components/ota_update/ota_update.c")
        fn = ota[ota.index("static void ota_do_update(const ota_request_t *r)"):]
        fn = fn[:fn.index("\n}\n")]
        entry = fn.index("if (refuse_for_power(r)) return;")
        accepted = fn.index('ota_report("accepted"')
        spread = fn.index("if (r->fleet_spread) wait_for_fleet_slot();")
        final = fn.index("if (refuse_for_power(r)) return;   /* 2. final admission */")
        started = fn.index("r->started(r->id, r->cb_ctx);")
        suspend = fn.index("s_cfg.workload_suspend();")
        self.assertTrue(entry < accepted < spread < final < started < suspend)
        # Round 2: admitted again after the suspend, immediately before the download.
        last = fn.index("bool power_late = power_protect_blocks_flash();")
        self.assertTrue(suspend < fn.index("s_cfg.comms_suspend();") < last
                        < fn.index("power_late ? ESP_FAIL : esp_https_ota_begin(&cfg, &h);"))
        self.assertLess(fn.index("power_late = true;"), fn.index("err = esp_https_ota_finish(h);"))
        refuse = ota[ota.index("static bool refuse_for_power(const ota_request_t *r)"):]
        refuse = refuse[:refuse.index("\n}\n")]
        self.assertIn('ota_report("dropped", r->id, "power_protect");', refuse)
        self.assertIn('r->finished(r->id, true, "power_protect", r->cb_ctx);', refuse)
        amb = self.read("components/ambit_ota/ambit_ota.c")
        run = amb[amb.index("static void ambit_ota_run(void *arg)"):]
        self.assertLess(run.index("power_blocks_write("), run.index("switch (r->op)"))
        attach = amb[amb.index("static void ambit_do_attach(void)"):]
        attach = attach[:attach.index("\n}\n")]
        self.assertLess(attach.rindex("if (power_protect_blocks_flash()) {"),
                        attach.index("esp_err_t e = ambit_flash_attach_flash(ch, &tgt);"))
        # Round 2: the writers themselves admit at the last safe point.
        impl = amb[amb.index("static bool ambit_ota_one_impl(uint8_t ch, const ambit_stage_t *stage, bool *power_refused)\n{"):]
        impl = impl[:impl.index("\n}\n")]
        self.assertTrue(impl.index('ambit_log_version(ch, "before");') < impl.index("power_protect_blocks_flash()")
                        < impl.index("ambit_stream_image(ch, stage)"))
        flash = self.read("components/ambit_flash/ambit_flash.c")
        gated = flash[flash.index("esp_err_t ambit_flash_image_gated("):]
        gated = gated[:gated.index("\nreset_and_done:")]
        order = [gated.index(k) for k in ("ambit_flash_preflight(", "uart_sensors_flash_session_begin(",
                                          "esp_loader_connect_with_stub(", "esp_loader_change_transmission_rate(",
                                          "if (may_write != NULL && !may_write()) {", "flash_one_region(")]
        self.assertEqual(order, sorted(order))
        # Every unattended writer is gated; the plain one is the USB console's.
        for rel in ("components/ambit_ota/ambit_ota.c", "components/ambit_flash/ambit_flash.c"):
            src = self.read(rel)
            calls = [l for l in src.splitlines() if "ambit_flash_image(" in l and "ambit_flash_image(uint8_t" not in l
                     and not l.strip().startswith(("*", "/*"))]
            self.assertEqual(calls, [], rel)
        self.assertIn("ambit_flash_image((uint8_t)ch, dir, baud, &r);", self.read("components/CLI/CLI.c"))
        self.assertLess(flash.index("if (power_protect_blocks_flash()) {"),
                        flash.index("if (!device_commands_flash_power_ok()) {"))
        loop = flash[flash.index("int flashed = 0;"):]
        self.assertLess(loop.index("if (power_protect_blocks_flash()) {"), loop.index("boot_fail_bump(ch, ver, fails[ch]);"))

    def test_command_and_pong_are_wired(self):
        src = self.read("components/command_router/command_router.c")
        self.assertIn("} else if (power_protect_command_handle(root)) {", src)
        self.assertIn("pp_state_name(power_protect_state())", src)
        self.assertIn(".get_status = power_protect_get_status", src)

    def test_heartbeat_carries_power_protect(self):
        src = self.read("components/device_commands/device_commands.c")
        self.assertIn("if (power_protect_render_json(pp, PP_JSON_CAP) > 0) input.power_protect_json = pp;", src)
        self.assertIn("malloc(DIAG_JSON_CAP + SDLOG_JSON_CAP + PP_JSON_CAP)", src)
        codec = self.read("components/payload_codec/payload_v3.c")
        self.assertIn('",\\"power_protect\\":%s", input->power_protect_json', codec)


if __name__ == "__main__":
    unittest.main()

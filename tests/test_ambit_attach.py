# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Attach-time AMBIT firmware gate: a newly plugged (or swapped, or reflashed)
AMBIT is not measured until its firmware was checked against the SD target and,
if older, updated.

Compiles the production components/device_commands/ambit_attach_gate.c into a
shared library and drives it with ctypes; source-contract tests pin the wiring
the host build cannot see (schedule gate, prefetch, checker, kick)."""

import ctypes
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLEAR, CLEAR_RECHECK, RETRY, FLASHED = 0, 1, 2, 3
RETRY_MS = 10 * 60 * 1000
A = b"3C:DC:75:0E:04:28"
B = b"3C:DC:75:0E:02:B8"


class PowerReading(ctypes.Structure):
    _fields_ = [("battery_mv", ctypes.c_uint16), ("system_mv", ctypes.c_uint16), ("input_mv", ctypes.c_uint16),
                ("charge_ma", ctypes.c_uint16), ("input_ma", ctypes.c_uint16), ("input_present", ctypes.c_bool),
                ("charge_status", ctypes.c_uint8), ("status_reg", ctypes.c_uint8), ("fault_reg", ctypes.c_uint8)]


def build(out: Path) -> Path:
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc") or "cc"
    lib = out / "libgate.so"
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    f"-I{ROOT / 'components/device_commands/include'}", f"-I{ROOT / 'components/domain/include'}",
                    f"-I{ROOT / 'tests/sd_shim/stubs'}",
                    str(ROOT / "components/device_commands/ambit_attach_gate.c"), "-o", str(lib)],
                   check=True, capture_output=True, text=True)
    return lib


class GateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.lib_path = build(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        # A fresh copy per test: the gate keeps file-scope state.
        p = Path(self.tmp.name) / f"g-{self._testMethodName}.so"
        shutil.copy(self.lib_path, p)
        self.g = ctypes.CDLL(str(p))
        self.g.ambit_attach_gate_allow.restype = ctypes.c_bool
        self.g.ambit_attach_gate_allow.argtypes = [ctypes.c_uint8, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int64]
        self.g.ambit_attach_gate_resolve.argtypes = [ctypes.c_uint8, ctypes.c_char_p, ctypes.c_char_p,
                                                     ctypes.c_int, ctypes.c_int64]
        self.g.ambit_attach_gate_pending.restype = ctypes.c_uint8
        self.g.ambit_attach_gate_held.restype = ctypes.c_bool
        self.g.ambit_attach_power_ok.restype = ctypes.c_bool
        self.kicks = 0
        self.kick_ok = True

        @ctypes.CFUNCTYPE(ctypes.c_bool)
        def kick():
            self.kicks += 1
            return self.kick_ok
        self._kick = kick
        self.g.ambit_attach_gate_set_kick(kick)

    def allow(self, ch, dev, fw, t=0):
        return self.g.ambit_attach_gate_allow(ch, dev, fw, t)

    def resolve(self, ch, dev, fw, v, t=0):
        self.g.ambit_attach_gate_resolve(ch, dev, fw, v, t)

    def test_new_ambit_is_held_until_cleared(self):
        self.assertFalse(self.allow(0, A, b"0.0.6"))
        self.assertTrue(self.g.ambit_attach_gate_held(0))
        self.assertEqual(self.g.ambit_attach_gate_pending(), 0b0001)
        self.assertEqual(self.kicks, 1)
        self.assertFalse(self.allow(0, A, b"0.0.6"), "still held while the check runs")
        self.assertEqual(self.kicks, 1, "one checker pass at a time")
        self.resolve(0, A, b"0.0.6", CLEAR)
        self.g.ambit_attach_gate_checker_done()
        self.assertTrue(self.allow(0, A, b"0.0.6"))
        self.assertFalse(self.g.ambit_attach_gate_held(0))

    def test_flash_makes_the_new_firmware_go_through_the_check(self):
        self.allow(0, A, b"0.0.6")
        self.resolve(0, A, b"0.0.6", FLASHED)
        self.g.ambit_attach_gate_checker_done()
        self.assertFalse(self.allow(0, A, b"0.0.6"), "an old pair never becomes cleared by a flash")
        self.assertFalse(self.allow(0, A, b"1.4.0"), "the reflashed unit is checked again")
        self.resolve(0, A, b"1.4.0", CLEAR)
        self.assertTrue(self.allow(0, A, b"1.4.0"))

    def test_swap_on_a_cleared_channel_is_held_again(self):
        self.allow(1, A, b"1.4.0")
        self.resolve(1, A, b"1.4.0", CLEAR)
        self.assertTrue(self.allow(1, A, b"1.4.0"))
        self.assertFalse(self.allow(1, B, b"1.4.0"), "a different AMBIT on the same port")
        self.assertFalse(self.allow(1, A, b"1.3.0"), "same AMBIT, different firmware")

    def test_low_power_retry_waits_ten_minutes_but_not_for_a_new_ambit(self):
        self.allow(2, A, b"0.0.6", 1000)
        self.resolve(2, A, b"0.0.6", RETRY, 1000)
        self.g.ambit_attach_gate_checker_done()
        self.assertFalse(self.allow(2, A, b"0.0.6", 1000 + RETRY_MS - 1))
        self.assertEqual(self.g.ambit_attach_gate_pending(), 0, "no re-check before the retry time")
        self.assertFalse(self.allow(2, A, b"0.0.6", 1000 + RETRY_MS))
        self.assertEqual(self.g.ambit_attach_gate_pending(), 0b0100, "re-checked after it")
        self.resolve(2, A, b"0.0.6", RETRY, 1000 + RETRY_MS)
        self.g.ambit_attach_gate_checker_done()
        self.assertFalse(self.allow(2, B, b"0.0.6", 1000 + RETRY_MS + 1))
        self.assertEqual(self.g.ambit_attach_gate_pending(), 0b0100, "another AMBIT is checked at once")

    def test_unknown_target_measures_and_rechecks(self):
        self.allow(3, A, b"0.0.6", 0)
        self.resolve(3, A, b"0.0.6", CLEAR_RECHECK, 0)
        self.g.ambit_attach_gate_checker_done()
        self.assertTrue(self.allow(3, A, b"0.0.6", 1))
        self.assertEqual(self.g.ambit_attach_gate_pending(), 0)
        self.assertTrue(self.allow(3, A, b"0.0.6", RETRY_MS), "still measured while re-checking")
        self.assertEqual(self.g.ambit_attach_gate_pending(), 0b1000)

    def test_unreadable_identity_is_never_held(self):
        self.assertTrue(self.allow(0, None, None))
        self.assertEqual((self.kicks, self.g.ambit_attach_gate_pending()), (0, 0))

    def test_full_worker_queue_is_retried_on_the_next_gate_call(self):
        self.kick_ok = False
        self.assertFalse(self.allow(0, A, b"0.0.6"))
        self.kick_ok = True
        self.assertFalse(self.allow(0, A, b"0.0.6"))
        self.assertEqual(self.kicks, 2)

    def test_power_rule(self):
        def ok(**kw):
            p = PowerReading(**kw)
            return self.g.ambit_attach_power_ok(ctypes.byref(p))
        self.assertTrue(ok(input_present=True))
        self.assertTrue(ok(battery_mv=3700))
        self.assertFalse(ok(battery_mv=3699))
        # MP2731: battery reads 0 when not charging; the system rail stands in.
        self.assertTrue(ok(battery_mv=0, system_mv=3900))
        self.assertFalse(ok(battery_mv=0, system_mv=3500))
        self.assertFalse(ok(battery_mv=0, system_mv=0), "no usable reading is not enough")


class WiringContractTest(unittest.TestCase):
    def setUp(self):
        self.act = (ROOT / "components/sched_runner/sched_runner_actions.c").read_text()

    def test_schedule_channels_go_through_the_gate(self):
        present = self.act.split("static bool ch_present(uint8_t ch)", 1)[1].split("}", 1)[0]
        self.assertIn("ch_gate(ch, true)", present)
        gate = self.act.split("static bool ch_gate(uint8_t ch, bool may_fetch)", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("ambit_attach_gate_allow(", gate)
        self.assertIn("cmd_ambit_device_info_cached(ch, &inf)", gate)

    def test_trace_trigger_loop_never_fetches_identity_on_the_wire(self):
        fn = self.act.split("sensor_inventory_veto_begin();", 1)[1]
        prefetch = fn.index("if (ch_ping(ch)) { ambit_device_info_t info; (void)cmd_ambit_device_info(ch, &info); }")
        trigger = fn.index("if (!ch_gate(ch, false)) continue;")
        self.assertLess(prefetch, trigger)

    def test_checker_is_wired_and_flashes_only_with_power(self):
        ota = (ROOT / "components/ambit_ota/ambit_ota.c").read_text()
        fn = ota.split("static void ambit_do_attach(void)", 1)[1].split("\n}\n", 1)[0]
        stale = fn.split("case AMBIT_ATTACH_DECIDE_STALE:", 1)[1]
        self.assertLess(stale.index("ambit_attach_power_ok(&p)"), stale.index("ambit_flash_attach_flash("))
        self.assertLess(stale.index("s_cfg.workload_suspend()"), stale.index("ambit_flash_attach_flash("))
        self.assertIn("ambit_attach_gate_checker_done();", fn)
        main = (ROOT / "main/app_main.c").read_text()
        self.assertIn("ambit_attach_gate_set_kick(ambit_ota_request_attach_check);", main)

    def test_attach_flash_shares_the_boot_fail_cap(self):
        fl = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        fn = fl.split("esp_err_t ambit_flash_attach_flash(", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(fn.index("boot_fail_bump(ch, ver, fails);"), fn.index("ambit_flash_image("))
        decide = fl.split("ambit_attach_decision_t ambit_flash_attach_decide(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("boot_fail_count(ch, ver) >= BOOT_FAIL_MAX", decide)
        self.assertIn("AMBIT_ATTACH_DECIDE_NEWER", decide)


if __name__ == "__main__":
    unittest.main()

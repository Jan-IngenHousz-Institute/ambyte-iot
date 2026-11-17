"""Maintenance vs charger-mutation admission (components/domain/include/maint_gate.h).

The production MP2731 adapter (one translation unit) with the real gate the
composition root uses, against a fake charger; tests/charger_gate_host.c plays
the maintenance worker. Covers every charger mutation: VIN/IIN set, AICO
on/off/reset/automatic re-arm, manual BG_EN and DP/DM re-detect, automatic
re-detect pulse. Runs under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_aico import FLAGS, compiler, ROOT, DEVICE_COMMANDS_STUB


class GateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="charger-gate-")
        directory = Path(cls.tmp.name)
        (directory / "device_commands.h").write_text(DEVICE_COMMANDS_STUB)
        cls.binary = directory / "gate"
        subprocess.run([compiler(), *FLAGS, "-DESP_ERR_TIMEOUT=0x107",
                        f"-I{directory}", f"-I{ROOT / 'tests/mp2731_driver_stubs'}",
                        f"-I{ROOT / 'tests/host_stubs'}",
                        *(f"-I{ROOT / 'components' / name / 'include'}" for name in ("domain", "mp2731")),
                        str(ROOT / "tests/charger_gate_host.c"),
                        str(ROOT / "components/mp2731/mp2731_core.c"),
                        str(ROOT / "components/mp2731/mp2731_diagnostics.c"),
                        str(ROOT / "components/mp2731/mp2731_redetect.c"),
                        str(ROOT / "components/mp2731/mp2731_aico.c"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.binary, case], capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result.stdout

    def test_maintenance_requested_at_every_io_step_waits_for_every_mutation(self):
        out = self.run_case("each_step")
        for name in ("vin_set", "iin_set", "aico_on", "aico_off", "aico_reset", "aico_auto",
                     "redetect_bg_en", "redetect_dpdm", "redetect_auto"):
            self.assertIn(f"{name}: ", out)

    def test_maintenance_held_refuses_commands_without_bus_access_and_holds_automatic_actions(self):
        self.run_case("held")

    def test_maintenance_beginning_right_after_a_veto_probe_blocks_the_write(self):
        self.run_case("race")

    def test_abandoned_request_readmits_mutations(self):
        self.run_case("abandon")

    def test_dpdm_restore_happens_before_maintenance_even_if_a_snapshot_times_out(self):
        self.run_case("dpdm_cleanup")

    def test_failed_dpdm_restore_quarantines_maintenance_until_recovery(self):
        self.run_case("quarantine_dpdm")

    def test_failed_dpdm_restore_with_maintenance_pending_drains_then_grants(self):
        self.run_case("quarantine_dpdm_pending")

    def test_dirty_bg_en_from_a_manual_pulse_quarantines_maintenance(self):
        self.run_case("quarantine_bg_en")

    def test_dirty_bg_en_from_an_automatic_pulse_quarantines_maintenance(self):
        self.run_case("quarantine_bg_en_auto")

    def test_no_bg_en_or_usb_det_en_write_from_any_read_path_under_granted_maintenance(self):
        self.run_case("no_edge_under_maintenance")

    def test_esp_reboot_with_usb_det_en_still_set_rebuilds_the_quarantine(self):
        self.run_case("reboot_dpdm")

    def test_esp_reboot_with_bg_en_still_set_rebuilds_the_quarantine(self):
        self.run_case("reboot_bg_en")

    def test_unreadable_register_at_boot_fails_closed_then_recovers(self):
        self.run_case("boot_read_failure")

    def test_clean_charger_at_boot_is_never_quarantined(self):
        self.run_case("boot_clean")

    def test_app_main_holds_maintenance_until_the_charger_is_reconciled(self):
        """Source contract (app_main is not host-built): the charger boot hold
        is taken before any maintenance-capable component is configured and
        released only after mp2731_init reconciled REG09/REG0B; the gate is
        wired before mp2731_init so the rebuilt quarantine is held at once."""
        src = (ROOT / "main/app_main.c").read_text()
        main = src[src.index("void app_main(void)"):]
        hold = main.index("app_charger_boot_hold_begin();")
        for user in ("command_router_init(", "ota_update_init(", "ambit_ota_init(",
                     "script_update_init("):
            if user in main:
                self.assertLess(hold, main.index(user), user)
        i2c = main.index("app_init_i2c_and_sensors();")
        end = main.index("app_charger_boot_hold_end();", i2c)
        # Released right after the reconciliation, before anything else runs.
        self.assertNotIn(";", main[i2c + len("app_init_i2c_and_sensors();"):end].replace(" */", "").split("/*")[0])
        self.assertLess(main.index("app_init_nvs();"), hold)
        init = src[src.index("static esp_err_t app_init_i2c_and_sensors(void)"):]
        init = init[:init.index("\n}\n")]
        self.assertLess(init.index("mp2731_set_mutation_gate("), init.index("mp2731_init()"))

    def test_pure_gate_semantics(self):
        self.run_case("gate")


if __name__ == "__main__":
    unittest.main()

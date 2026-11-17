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

    def test_pure_gate_semantics(self):
        self.run_case("gate")


if __name__ == "__main__":
    unittest.main()

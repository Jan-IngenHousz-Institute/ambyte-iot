"""Source-contract tests for remote AMBIT ROM recovery bootstrap."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AmbitFlashBootstrapTest(unittest.TestCase):
    def test_missing_recovery_directories_are_created_before_file_preflight(self):
        source = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        function = source.split("esp_err_t ambit_flash_image(", 1)[1]
        create_root = function.index("mkdir(AMBIT_FW_ROOT, 0777)")
        create_version = function.index("mkdir(dir, 0777)")
        file_preflight = function.index("for (size_t i = 0; i < NUM_REGIONS; i++)")
        take_uart = function.index("uart_sensors_flash_session_begin")
        self.assertLess(create_root, create_version)
        self.assertLess(create_version, file_preflight)
        self.assertLess(file_preflight, take_uart)
        self.assertIn("errno != EEXIST", function[create_root:file_preflight])

    def test_successful_rom_flash_invalidates_cached_ambit_identity(self):
        source = (ROOT / "components/ambit_ota/ambit_ota.c").read_text()
        function = source.split("static void ambit_do_flash(", 1)[1]
        function = function.split("static void ambit_ota_run(", 1)[0]
        self.assertIn("cmd_ambit_device_info_invalidate(c);", function)
        self.assertIn("cmd_ambit_device_info_invalidate(r->channel);", function)
        self.assertLess(
            function.index("cmd_ambit_device_info_invalidate(c);"),
            function.index("s_cfg.comms_resume()"),
        )


if __name__ == "__main__":
    unittest.main()

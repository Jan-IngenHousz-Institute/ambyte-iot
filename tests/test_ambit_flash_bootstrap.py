# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Source-contract tests for remote AMBIT ROM recovery bootstrap."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AmbitFlashBootstrapTest(unittest.TestCase):
    def test_boot_sync_never_downgrades_newer_ambit_firmware(self):
        source = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        boot_sync = source.split("int ambit_flash_boot_sync(void)", 1)[1]

        self.assertIn("CH_NEWER", boot_sync)
        self.assertEqual(
            boot_sync.count("fw_is_newer_than_target(&fw, &tgt)"),
            2,
        )
        self.assertIn("newer than SD target", boot_sync)

        flash_loop = boot_sync.split(
            "/* Power gate", 1
        )[1].split("return flashed;", 1)[0]
        self.assertNotIn("state[ch] == CH_NEWER", flash_loop)

    def test_missing_recovery_directories_are_created_before_file_preflight(self):
        # The mkdirs + region preflight moved into ambit_flash_preflight(), which
        # runs inside one SD io bracket before the UART bus is taken.
        source = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        function = source.split("esp_err_t ambit_flash_image(", 1)[1]
        preflight_call = function.index("ambit_flash_preflight(AMBIT_FW_ROOT, dir, fnames, NUM_REGIONS)")
        take_uart = function.index("uart_sensors_flash_session_begin")
        self.assertLess(preflight_call, take_uart)

        pre = (ROOT / "components/ambit_flash/ambit_flash_preflight.c").read_text()
        body = pre.split("esp_err_t ambit_flash_preflight(", 1)[1]
        gate = body.index("sdcard_io_begin()")
        create_root = body.index("mkdir(root, 0777)")
        create_version = body.index("mkdir(dir, 0777)")
        file_preflight = body.index("for (size_t i = 0; i < n; i++)")
        release = body.index("sdcard_io_end()")
        self.assertLess(gate, create_root)
        self.assertLess(create_root, create_version)
        self.assertLess(create_version, file_preflight)
        self.assertLess(file_preflight, release)
        self.assertIn("errno != EEXIST", body[create_root:file_preflight])

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

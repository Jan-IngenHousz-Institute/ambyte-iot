"""Behavior and composition checks for persisted AMBIT announcements."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AmbitAnnouncementTest(unittest.TestCase):
    def test_tracker_uses_injected_store_and_survives_reinitialization(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ambit-announcement-host-") as tmp:
            binary = Path(tmp) / "ambit_announcement_host"
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'components/domain/include'}",
                    f"-I{ROOT / 'components/ambit_announcement/include'}",
                    f"-I{ROOT / 'tests/host_stubs'}",
                    str(ROOT / "tests/ambit_announcement_host.c"),
                    str(ROOT / "components/ambit_announcement/ambit_announcement.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True, cwd=ROOT
            )
            self.assertIn("tracker tests: ok", result.stdout)

    def test_nvs_is_composed_outside_device_commands(self) -> None:
        source = (ROOT / "components/device_commands/device_commands.c").read_text()
        header = (ROOT / "components/device_commands/include/device_commands.h").read_text()
        app = (ROOT / "main/app_main.c").read_text()
        cmake = (ROOT / "components/device_commands/CMakeLists.txt").read_text()

        self.assertNotIn('#include "nvs.h"', source)
        self.assertNotIn("nvs_flash", cmake)
        self.assertIn("ambit_announcement_store_port_t announcement_store", header)
        self.assertIn("ambit_announcement_init", source)
        self.assertIn("ambit_announcement_commit", source)
        self.assertLess(source.index("s_cfg.store_event(&d)"),
                        source.index("ambit_announce_persist(announce_slot, e)"))
        self.assertIn("ambit_announcement_nvs_port()", app)

    def test_domain_component_remains_header_only(self) -> None:
        cmake = (ROOT / "components/domain/CMakeLists.txt").read_text()
        self.assertNotIn("SRCS", cmake)
        self.assertEqual(list((ROOT / "components/domain").glob("*.c")), [])


if __name__ == "__main__":
    unittest.main()

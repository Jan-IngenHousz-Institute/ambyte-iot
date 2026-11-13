# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Bench flashing preserves identity and uses the actual IDF layout."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from contextlib import contextmanager
from types import SimpleNamespace
from tools.bench_flash import flash_layout

ROOT = Path(__file__).resolve().parents[1]


def load():
    spec = importlib.util.spec_from_file_location("bench_flash", ROOT / "tools/bench_flash.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class BenchFlashTest(unittest.TestCase):
    @contextmanager
    def tool(self, regenerate=True):
        mod = load()
        with tempfile.TemporaryDirectory() as directory:
            mod.ROOT=Path(directory)
            build=mod.ROOT/".pio/build/esp32-s3-devkitm-1";build.mkdir(parents=True)
            (build/"nvs.bin").write_bytes(b"stale credential image")
            table=build/"partitions.bin";table.write_bytes(b"partition fixture")
            app=build/"firmware.bin"
            images=[(0x8000,table),(0x20000,app)]
            events=[]
            def run(command, **kwargs):
                events.append(command)
                if command[0]=="pio":
                    app.write_bytes(b"fresh app")
                    if "AMBYTE_NVS_SKIP" not in kwargs["env"]:
                        self.assertFalse((build/"nvs.bin").exists(),"stale NVS must be deleted before build")
                        if regenerate:(build/"nvs.bin").write_bytes(b"fresh identity")
            def flash(*args,**kwargs):events.append("flash")
            with mock.patch.object(mod.subprocess,"run",side_effect=run) as calls, \
                 mock.patch.object(mod,"flash_layout",return_value=(images,{})), \
                 mock.patch.object(mod,"validate_signed_build",side_effect=lambda b:events.append("validate")), \
                 mock.patch("serial.tools.list_ports.comports",return_value=[SimpleNamespace(device="/dev/ttyACM9",serial_number="AA:BB:CC:DD:EE:FF")]), \
                 mock.patch("flash_gui.esptool_ops.read_mac") as mac, \
                 mock.patch("flash_gui.esptool_ops.flash_images",side_effect=flash) as writer:
                yield mod,calls,writer,mac,events,table

    def test_default_preserves_nvs_for_build_and_upload(self):
        with self.tool() as (mod,calls,writer,mac,events,table):
            self.assertEqual(mod.main(["--port","/dev/ttyACM9"]),0)
            builds=[c for c in calls.call_args_list if c.args[0][0]=="pio"]
            self.assertEqual(len(builds),1)
            self.assertEqual(builds[0].kwargs["env"].get("AMBYTE_NVS_SKIP"),"1")
            self.assertFalse(any("upload" in c.args[0] for c in builds))
            self.assertEqual(writer.call_args.kwargs["expected_partition_table"],table.read_bytes())
            self.assertEqual(writer.call_args.kwargs["expected_mac"],"AA:BB:CC:DD:EE:FF")
            self.assertEqual(writer.call_args.args[1][-1][0],0x20000)

    def test_provision_writes_nvs_even_if_the_shell_set_skip(self):
        with self.tool() as (mod,calls,writer,mac,events,table), mock.patch.dict("os.environ",{"AMBYTE_NVS_SKIP":"1"}):
            self.assertEqual(mod.main(["--port","/dev/ttyACM9","--provision"]),0)
            build=next(c for c in calls.call_args_list if c.args[0][0]=="pio")
            self.assertNotIn("AMBYTE_NVS_SKIP",build.kwargs["env"])
            self.assertIsNone(writer.call_args.kwargs["expected_partition_table"])

    def test_always_bench_signs_before_upload(self):
        with self.tool() as (mod,calls,writer,mac,events,table):
            self.assertEqual(mod.main(["--port","/dev/ttyACM9"]),0)
            sign=next(i for i,c in enumerate(events) if isinstance(c,list) and any("sign_firmware.py" in str(a) for a in c))
            self.assertLess(sign,events.index("flash"))
            self.assertIn("--allow-bench",events[sign])
            self.assertTrue(str(writer.call_args.args[1][-1][1]).endswith("firmware.bin"))

    def test_missing_usb_mac_serial_requires_explicit_expected_mac(self):
        with self.tool() as (mod,calls,writer,mac,events,table), mock.patch("serial.tools.list_ports.comports",return_value=[]):
            with self.assertRaises(SystemExit) as error:
                mod.main(["--port","/dev/ttyACM9"])
            self.assertEqual(error.exception.code,2)
            calls.assert_not_called();writer.assert_not_called()

    def test_explicit_mac_supports_drivers_without_mac_serial(self):
        with self.tool() as (mod,calls,writer,mac,events,table), mock.patch("serial.tools.list_ports.comports",return_value=[]):
            self.assertEqual(mod.main(["--port","/dev/ttyACM9","--expected-mac","aa:bb:cc:dd:ee:ff"]),0)
            self.assertEqual(writer.call_args.kwargs["expected_mac"],"AA:BB:CC:DD:EE:FF")
            mac.assert_not_called()

    def test_skipped_provision_build_cannot_reuse_stale_nvs(self):
        with self.tool(regenerate=False) as (mod,calls,writer,mac,events,table):
            with self.assertRaisesRegex(RuntimeError,"fresh NVS"):
                mod.main(["--port","/dev/ttyACM9","--provision"])
            mac.assert_not_called();writer.assert_not_called()
            self.assertEqual(len(calls.call_args_list),1)


    def test_effective_config_is_validated_after_signing_before_the_port(self):
        with self.tool() as (mod,calls,writer,mac,events,table):
            self.assertEqual(mod.main(["--port","/dev/ttyACM9"]),0)
            sign=next(i for i,c in enumerate(events) if isinstance(c,list) and any("sign_firmware.py" in str(a) for a in c))
            self.assertLess(sign,events.index("validate"))
            self.assertLess(events.index("validate"),events.index("flash"))


class BenchFlashGuardTest(unittest.TestCase):
    """The real upload guard refuses a dangerous effective sdkconfig before
    esptool ever opens the port (review of #98: direct flash bypassed it)."""

    def test_dangerous_config_refused_before_any_port_access(self):
        mod = load()
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "config").mkdir()
            from flash_gui.firmware_metadata import REQUIRED
            good = {key: True for key in REQUIRED}
            for bad in ({**good, "SECURE_DISABLE_ROM_DL_MODE": True},
                        {k: v for k, v in good.items() if k != sorted(REQUIRED)[0]}):
                (build / "config/sdkconfig.json").write_text(json.dumps(bad))
                (build / "firmware.bin").write_bytes(b"irrelevant: config is checked first")
                with mock.patch("flash_gui.esptool_ops.flash_images") as writer, \
                     mock.patch("flash_gui.esptool_ops.read_mac") as mac:
                    with self.assertRaisesRegex(RuntimeError, "UPLOAD REFUSED"):
                        mod.validate_signed_build(build)
                    writer.assert_not_called()
                    mac.assert_not_called()


class BenchFlashLayoutTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.app = self.build / "signed.bin"
        for name in ("signed.bin","bootloader.bin","partitions.bin","ota_data_initial.bin"):
            (self.build/name).write_bytes(b"image")
        (self.build/"nvs.bin").write_bytes(b"\xff"*0x6000)
        self.manifest = {
            "app": {"offset": "0x20000", "file": "app.bin"},
            "flash_files": {"0x0":"bootloader.bin","0x8000":"partitions.bin","0xf000":"ota_data_initial.bin","0x20000":"app.bin"},
            "flash_settings": {"flash_mode":"dio","flash_size":"keep","flash_freq":"80m"},
        }

    def write(self):
        (self.build / "flasher_args.json").write_text(json.dumps(self.manifest))

    def test_signed_app_uses_idf_address_and_keeps_settings_partitions(self):
        self.write()
        images, settings = flash_layout(self.build, self.app, False)
        self.assertEqual(images, [(0,self.build/"bootloader.bin"),(0x8000,self.build/"partitions.bin"),
                                  (0xf000,self.build/"ota_data_initial.bin"),(0x20000,self.app)])
        self.assertEqual(settings,self.manifest["flash_settings"])
        self.assertTrue(all(offset not in (0x9000,0x11000,0x620000,0x6a0000) for offset,_ in images))

    def test_nvs_only_added_for_explicit_provision(self):
        self.write()
        self.assertIn((0x9000,self.build/"nvs.bin"),flash_layout(self.build,self.app,True)[0])

    def test_board_default_or_unrecognised_layout_refused(self):
        self.manifest["app"]["offset"]="0x10000"
        self.manifest["flash_files"]["0x10000"]=self.manifest["flash_files"].pop("0x20000")
        self.write()
        with self.assertRaisesRegex(RuntimeError,"Unsupported bench layout"):
            flash_layout(self.build,self.app,False)

    def test_erase_rounding_cannot_reach_preserved_nvs(self):
        self.write()
        (self.build/"bootloader.bin").write_bytes(b"x"*0x8001)
        with self.assertRaisesRegex(RuntimeError,"overlaps"):
            flash_layout(self.build,self.app,False)


if __name__ == "__main__":
    unittest.main()

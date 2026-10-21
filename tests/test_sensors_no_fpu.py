"""A read-only snapshot must not permanently pin the caller through FPU use."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_sensor_no_fpu import check_object, fpu_instructions


class SensorFormatterFpuTest(unittest.TestCase):
    def test_target_formatter_and_scanner(self):
        self.assertEqual(fpu_instructions("0: 030103 lsi f0, a1, 12\n3: fa1010 abs.s f1, f0"),
                         ["0: 030103 lsi f0, a1, 12", "3: fa1010 abs.s f1, f0"])
        self.assertEqual(fpu_instructions("0: 002136 entry a1, 16\n3: 0228 l32i.n a2, a2, 0"), [])
        self.assertEqual(fpu_instructions("0: 00c256 beqz a2, f1 <string+0xf1>"), [])
        pie = "0: 1c0401 ee.vld.128.ip q0, a1, 16"
        self.assertEqual(fpu_instructions(pie), [pie])
        build = ROOT / ".pio/build/esp32-s3-devkitm-1/components"
        objects = [build / "sensor_inventory/sensor_json.c.o", build / "sensor_inventory/sensor_inventory.c.o",
                   build / "CLI/cli_sensors.c.o", *sorted((build / "usb_service").glob("*.o"))]
        for obj in objects:
            if obj.exists():
                with self.subTest(object=obj.name):
                    self.assertEqual(check_object(obj), [], "inspection must not pin the console task")

    def test_wire_float_widening_preserves_bits_and_existing_decimal_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/"float"
            subprocess.run([os.environ.get("CC","clang"),"-std=c11","-Wall","-Wextra","-Werror",
                "-fsanitize=address,undefined","-fno-omit-frame-pointer","-g","-O1",
                f"-I{ROOT/'components/sensor_inventory/include'}",str(ROOT/'tests/sensors_host/float_driver.c'),
                "-o",str(binary)],check=True)
            run=subprocess.run([binary],capture_output=True,timeout=15)
            self.assertEqual(run.returncode,0,run.stderr.decode())
            self.assertIn(b"exact_bits=1 identical_json=1",run.stdout)


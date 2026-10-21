# SPDX-License-Identifier: GPL-3.0-or-later
"""Python client <-> complete production C snapshot provider and dispatcher."""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

from flash_gui.service_client import ServiceClient
from flash_gui.service_wire import ServiceError
from test_svc_python_client import PipeSerial

ROOT = Path(__file__).resolve().parents[1]


class SensorsServiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="sensor-service-")
        cls.binary = Path(cls.build.name) / "peer"
        includes = [ROOT / "tests/svc_host/include", ROOT / "tests/identity_host/include",
                    ROOT / "tests/wifi_manager_stubs"] + list((ROOT / "components").glob("*/include"))
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-DSVC_HOST", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *(f"-I{p}" for p in includes),
            *(str(ROOT / "components/usb_service" / p) for p in
              ("svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c", "svc_session.c")),
            str(ROOT / "components/sensor_inventory/sensor_inventory.c"),
            str(ROOT / "components/sensor_inventory/sensor_json.c"),
            str(ROOT / "tests/sensors_host/service_peer.c"), "-Wl,--wrap=svc_store_open",
            "-lcrypto", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.port = PipeSerial(self.binary, Path(self.temp.name) / "svc")
        self.addCleanup(self.port.close)
        self.client = ServiceClient(self.port, None)
        self.client.connect()

    def test_real_provider_client_bounds_and_cleanup(self):
        result = self.client.sensors()
        self.assertEqual([p.state for p in result.ports], ["known"] * 4)
        self.assertGreater(result.refresh["pin_heap_bytes"], 0)
        self.assertLessEqual(result.refresh["pin_heap_bytes"], 4096)
        self.assertEqual(self.client._exchange("svc.version")["highest_id"], "0000000000000000")
        self.client.exit()
        self.assertIn("readonly=1", self.port.close())

    def test_live_swap_cannot_tear_pin_and_new_pin_supersedes_old(self):
        header = self.client._exchange("svc.sensors")
        self.port.write(b"@swap\n")
        raw = bytearray()
        for offset in range(0, header["size"], 576):
            page = self.client._exchange(f"svc.sensors.read {header['snapshot']} {offset:08x}")
            block = base64.b64decode(page["data"], validate=True)
            self.assertEqual(page["crc32"], f"{zlib.crc32(block):08x}")
            raw.extend(block)
        self.assertEqual(hashlib.sha256(raw).hexdigest(), header["sha256"])
        self.assertEqual(json.loads(raw)["ports"][0]["sensor_id"]["value"], "01:00:00:00:00:00")
        new = self.client.sensors()
        self.assertEqual(new.ports[0].generation, 2)
        self.assertEqual(new.ports[0].observations["sensor_id"].value, "2A:00:00:00:00:00")
        self.assertIsNone(new.ports[0].observations["calibration"].value)
        with self.assertRaises(ServiceError) as error:
            self.client._exchange(f"svc.sensors.read {header['snapshot']} 00000000")
        self.assertEqual(error.exception.reason, "snapshot_expired")

    def test_expiry_and_invalid_offsets_are_correlated_errors(self):
        head = self.client._exchange("svc.sensors")
        for offset in (head["size"], head["size"] + 1, 0xffffffff):
            with self.assertRaises(ServiceError):
                self.client._exchange(f"svc.sensors.read {head['snapshot']} {offset:08x}")
        self.port.write(b"@advance\n")
        with self.assertRaises(ServiceError) as error:
            self.client._exchange(f"svc.sensors.read {head['snapshot']} 00000000")
        self.assertEqual(error.exception.reason, "snapshot_expired")
        self.assertEqual(len(self.client.sensors().ports), 4)


if __name__ == "__main__":
    unittest.main()

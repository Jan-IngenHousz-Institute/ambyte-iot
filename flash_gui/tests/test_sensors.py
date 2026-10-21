# SPDX-License-Identifier: GPL-3.0-or-later
"""Real serial/client parsers and the operator widget seam, using C snapshots."""
import base64
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

from flash_gui import ambyte_serial, gui, sensor_diagnostics
from flash_gui.sensors import decode_sensors, SensorsError
from flash_gui.service_client import ServiceClient, ProtocolError

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "flash_gui/tests"))
from service_device import Clock, Device, Serial


def identity_bytes(sensors=True):
    # Deliberately a generated unit fixture, not the pending real A3 capture.
    return json.dumps({"mac": "02:00:00:00:00:01", "thing_name": "bench", "fw": "a4",
        "contract": 1, "schedule": {"source": "NONE", "sha256": None,
        "workbookVersionId": None, "script_version": None},
        "capabilities": ["identity", "svc"] + (["sensors"] if sensors else [])}).encode()


class PlainSerial:
    def __init__(self, snapshot, identity):
        self.snapshot, self.identity = snapshot, identity
        self.output = bytearray()
        self.commands = []
        self.dtr = self.rts = True
        self.closed = False
        self.now = 0.0

    def open(self):
        assert not self.dtr and not self.rts

    def close(self):
        self.closed = True

    def reset_input_buffer(self):
        self.output.clear()

    def write(self, raw):
        command = raw.decode().strip()
        self.commands.append(command)
        if command == "identity":
            self.output.extend(self.identity + b"\r\r\nambyte> ")
        elif command == "sensors":
            self.output.extend(b"sensors\r\nI (1) worker: log\r\n" + self.snapshot + b"\r\r\nambyte> ")
        elif command == "status":
            self.output.extend(b" - MAC: 02:00:00:00:00:01\r\nambyte> ")
        else:
            raise AssertionError(command)
        return len(raw)

    def read(self, count):
        self.now += .001 if self.output else .1
        data = bytes(self.output[:min(count, 47)])
        del self.output[:len(data)]
        return data


class SnapshotDevice(Device):
    def __init__(self, snapshot):
        super().__init__()
        self.snapshot = snapshot

    def answer(self, body):
        if body == "svc.version":
            result = super().answer(body)
            result["actions"] += ["svc.sensors", "svc.sensors.read"]
            return result
        base = dict(ok=True, store=self.store)
        if body == "svc.sensors":
            return dict(base, snapshot="1111111111111111", size=len(self.snapshot),
                        sha256=hashlib.sha256(self.snapshot).hexdigest(), chunk=576)
        if body.startswith("svc.sensors.read "):
            _, token, offset = body.split()
            start = int(offset, 16)
            chunk = self.snapshot[start:start + 576]
            return dict(base, snapshot=token, offset=offset, crc32=f"{zlib.crc32(chunk):08x}",
                        data=base64.b64encode(chunk).decode(), eof=start + len(chunk) == len(self.snapshot))
        return super().answer(body)


class SensorClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="gui-sensor-")
        cls.binary = Path(cls.temp.name) / "sensors"
        spec = importlib.util.spec_from_file_location("_sensor_build", ROOT / "tests/test_sensors.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.build(cls.binary)
        cls.raw = subprocess.check_output([cls.binary, "filled"]).strip()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def console(self, identity=None, raw=None):
        port = PlainSerial(self.raw if raw is None else raw, identity_bytes() if identity is None else identity)
        self.enterContext(patch.object(ambyte_serial.serial, "Serial", return_value=port))
        self.enterContext(patch.object(ambyte_serial.time, "time", side_effect=lambda: port.now))
        return ambyte_serial.AmbyteConsole("bench"), port

    def test_real_console_physical_lines_split_chunks_and_typed_model(self):
        console, port = self.console()
        snapshot = console.sensors()
        self.assertEqual([p.state for p in snapshot.ports], ["known"] * 4)
        self.assertEqual(port.commands, ["identity", "sensors"])
        self.assertEqual(snapshot.ports[0].observations["calibration"].value["actinic_coef"], .25)
        with self.assertRaises(TypeError):
            snapshot.ports[0].observations["calibration"].value["actinic_coef"] = 0

    def test_no_sensors_token_and_real_250_capture_are_unavailable(self):
        console, port = self.console(identity=identity_bytes(False))
        self.assertIsNone(console.sensors())
        self.assertEqual(port.commands, ["identity"])
        old = (ROOT / "flash_gui/tests/fixtures/identity-2.5.0.raw").read_bytes()
        console, port = self.console(identity=old)
        self.assertIsNone(console.sensors())
        self.assertEqual(port.commands, ["identity", "status"])

    def test_corruption_never_downgrades(self):
        for value in (b'{"contract":1}', b"ambyte> " + self.raw, self.raw + b"log tail", b"", self.raw.replace(b'"port":0', b'"port":true')):
            with self.subTest(value=value[:24]):
                console, port = self.console(raw=value)
                with self.assertRaises(ambyte_serial.ConsoleError):
                    console.sensors(timeout=.5)
                self.assertNotIn("status", port.commands)

    def test_strict_schema_mutations_and_bounds(self):
        obj = json.loads(self.raw)
        mutations = [lambda s: s.update(contract=True), lambda s: s.update(boot_id="0"*16),
            lambda s: s["ports"].reverse(), lambda s: s["ports"][0].update(generation=True),
            lambda s: s["ports"][0]["spectral"]["value"].update(spec_sens=[0]*9),
            lambda s: s["ports"][0]["calibration_digest"]["value"].update(hex="0"*8),
            lambda s: s["ports"][0]["sensor_id"].update(source="last_read")]
        for mutate in mutations:
            value = copy.deepcopy(obj)
            mutate(value)
            with self.assertRaises(SensorsError):
                decode_sensors(json.dumps(value))
        for raw in (b'{"contract":1,"contract":1}', b"{" + b" "*16384 + b"}", self.raw.replace(b'"contract":1', b'"contract":NaN')):
            with self.assertRaises(SensorsError):
                decode_sensors(raw)

    def test_framed_shared_client_reassembles_actual_c_snapshot(self):
        clock = Clock()
        board = SnapshotDevice(self.raw)
        serial = Serial(board, clock, split=7)
        client = ServiceClient(serial, None, clock=clock, sleep=clock.sleep)
        self.assertEqual(client.sensors().as_dict(), json.loads(self.raw))
        self.assertEqual(board.highest, 0)
        self.assertGreater(serial.counts["svc.sensors.read"], 1)
        client.exit()

    def test_framed_wrong_pin_crc_digest_and_expiry_fail(self):
        for field, bad in (("snapshot", "2222222222222222"), ("crc32", "00000000"), ("offset", "00000001"), ("eof", "yes")):
            clock = Clock()
            def mutate(action, result):
                if action == "svc.sensors.read":
                    result[field] = bad
                return result
            client = ServiceClient(Serial(SnapshotDevice(self.raw), clock, mutate=mutate), None, clock=clock, sleep=clock.sleep)
            with self.subTest(field=field), self.assertRaises(ProtocolError):
                client.sensors()

    def test_diagnostics_callable_closes_serial_and_binds_four_widgets(self):
        console, serial = self.console()
        with patch.object(sensor_diagnostics, "connect_after_boot", return_value=console):
            model = sensor_diagnostics.read_sensor_diagnostics("bench", log=lambda _: None)
        self.assertTrue(serial.closed)
        app = object.__new__(gui.App)
        class Tree:
            def __init__(self): self.rows, self.selected = {}, ()
            def get_children(self): return list(self.rows)
            def delete(self, row): del self.rows[row]
            def insert(self, parent, where, *, iid, values): self.rows[iid] = values
            def selection_set(self, row): self.selected = (row,)
            def selection(self): return self.selected
        class Text:
            def configure(self, **kwargs): pass
            def delete(self, *args): self.value = ""
            def insert(self, where, text): self.value = text
        class Value:
            def set(self, value): self.value = value
        app.sensor_tree, app.sensor_details, app.sensor_message = Tree(), Text(), Value()
        app._apply_sensor_diagnostics(model)
        self.assertEqual(list(app.sensor_tree.rows), ["0", "1", "2", "3"])
        self.assertIn("calibration", app.sensor_details.value)
        self.assertIn("sntp", app.sensor_details.value)
        self.assertIn("Last observed", app.sensor_message.value)
        app._apply_sensor_diagnostics(sensor_diagnostics.SensorDiagnostics(None, "unavailable"))
        self.assertEqual(app.sensor_tree.rows, {})
        self.assertEqual(app.sensor_details.value, "unavailable")

    def test_released_a2_parser_accepts_both_additive_tokens(self):
        source = subprocess.check_output(["git", "show", "6b86c8e33fc19149f4c5661545143653aa919705:flash_gui/ambyte_serial.py"], cwd=ROOT)
        path = Path(self.temp.name) / "released.py"
        path.write_bytes(source)
        spec = importlib.util.spec_from_file_location("flash_gui._released_a2_serial", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        try:
            spec.loader.exec_module(module)
            parsed = module._identity_object(json.loads(identity_bytes()))
            self.assertEqual(parsed.capabilities, frozenset({"identity", "svc", "sensors"}))
            self.assertEqual(parsed.mac, "02:00:00:00:00:01")
        finally:
            sys.modules.pop(spec.name, None)


if __name__ == "__main__":
    unittest.main()

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
from unittest.mock import patch, Mock
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

    def test_stalled_snapshot_is_distinct_on_diagnostics_widget_seam(self):
        raw = subprocess.check_output([self.binary, "stalled"])
        console, serial = self.console(raw=raw)
        snapshot = console.sensors()
        self.assertEqual(snapshot.ports[0].state, "stalled")
        self.assertGreater(snapshot.as_dict()["acquisition_recovery"]["wire_transactions"][0], 0)
        model = sensor_diagnostics.SensorDiagnostics(snapshot, "observed")
        app = object.__new__(gui.App)
        app.sensor_message, app.sensor_tree, app.sensor_details = Mock(), Mock(), Mock()
        app.sensor_tree.get_children.return_value = ()
        app.sensor_tree.selection.return_value = ("0",)
        app._apply_sensor_diagnostics(model)
        rows = [call.kwargs["values"] for call in app.sensor_tree.insert.call_args_list]
        self.assertEqual(len(rows), 4)
        self.assertIn("stalled — completion unproven", rows[0][1])
        self.assertNotIn("absent", rows[0][1])
        self.assertIn('"state": "stalled"', sensor_diagnostics.diagnostic_details(model, 0))

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

    def test_descriptor_page_digest_and_transport_faults_invalidate_client(self):
        faults = (("svc.sensors", "size", 16385), ("svc.sensors", "chunk", True),
                  ("svc.sensors", "snapshot", "0" * 16),
                  ("svc.sensors", "sha256", "0" * 64),
                  ("svc.sensors.read", "data", "!"),
                  ("svc.sensors.read", "data", ""),
                  ("svc.sensors.read", "eof", True))
        for when, field, bad in faults:
            clock = Clock()
            def mutate(action, result):
                if action == when:
                    result[field] = bad
                return result
            client = ServiceClient(Serial(SnapshotDevice(self.raw), clock, mutate=mutate),
                                   None, clock=clock, sleep=clock.sleep)
            with self.subTest(when=when, field=field), self.assertRaises(ProtocolError):
                client.sensors()
            self.assertFalse(client.active)
        # An unplug after pinning is a transport failure, never an empty model.
        clock = Clock()
        board = SnapshotDevice(self.raw)
        serial = Serial(board, clock)
        original = serial.write
        def disconnect(data):
            if b"svc.sensors.read" in data:
                raise OSError("unplugged")
            return original(data)
        serial.write = disconnect
        client = ServiceClient(serial, None, clock=clock, sleep=clock.sleep)
        from flash_gui.service_client import TransportError
        with self.assertRaises(TransportError):
            client.sensors(timeout=.2)
        self.assertFalse(client.active)

    def test_diagnostics_waits_on_same_connection_for_post_boot_observations(self):
        cold = decode_sensors(subprocess.check_output([self.binary,"empty"]).strip())
        partial = decode_sensors(subprocess.check_output([self.binary,"reconnect"]).splitlines()[-1])
        ready = decode_sensors(self.raw)
        scenarios = (((cold,ready),2,0,"Last observed"),
                     ((cold,),20,0,"No attachments have been observed since this gateway boot"),
                     ((partial,),20,0,"Some ports still lack identity or calibration observations"),
                     ((cold,),2,44,"No attachments have been observed since this gateway boot"))
        for transitions, expected_reads, boot_seconds, message in scenarios:
            with self.subTest(message=message,boot_seconds=boot_seconds):
                console, serial = self.console()
                ticks = [0.0]
                def sleep(seconds): ticks[0] += seconds
                def connected(*args,**kwargs):
                    ticks[0] += boot_seconds
                    return console
                reader = Mock(side_effect=list(transitions)) if len(transitions)>1 else Mock(return_value=transitions[0])
                with patch.object(sensor_diagnostics,"connect_after_boot",side_effect=connected) as connect, \
                     patch.object(console,"sensors",reader), \
                     patch.object(sensor_diagnostics.time,"monotonic",side_effect=lambda: ticks[0]), \
                     patch.object(sensor_diagnostics.time,"sleep",side_effect=sleep):
                    model = sensor_diagnostics.read_sensor_diagnostics("bench",log=lambda _: None)
                self.assertTrue(serial.closed)
                self.assertEqual(connect.call_count,1)
                self.assertEqual(reader.call_count,expected_reads)
                self.assertLessEqual(ticks[0],min(boot_seconds+10,45))
                self.assertIn(message,model.message)
                if len(transitions)>1:
                    self.assertIs(model.snapshot,ready)
                else:
                    self.assertIn("idle window",model.message)

    def test_diagnostics_failure_always_closes_serial(self):
        console, serial = self.console(raw=b"{malformed}")
        with patch.object(sensor_diagnostics, "connect_after_boot", return_value=console):
            with self.assertRaises(ambyte_serial.ConsoleError):
                sensor_diagnostics.read_sensor_diagnostics("bench", log=lambda _: None)
        self.assertTrue(serial.closed)

    def test_gui_worker_survives_close_and_reports_errors_on_visible_surface(self):
        for outcome in ("close_window", "new_window", "close_app", "failure"):
            with self.subTest(outcome=outcome):
                app = object.__new__(gui.App)
                app.busy = False
                app._sensor_view_generation = 1
                app.sensor_window = Mock()
                app.sensor_window.winfo_exists.return_value = True
                app.sensor_message, app.sensor_details, app.sensor_tree = Mock(), Mock(), Mock()
                app.sensor_tree.get_children.return_value = ("old",)
                app.log = Mock()
                app._selected_port = lambda: "bench"
                app._open_sensor_panel = Mock()
                app._apply_sensor_diagnostics = Mock()
                app._set_busy = lambda busy: setattr(app, "busy", busy)
                callbacks, jobs = [], []
                app._post = lambda fn, *args: callbacks.append((fn, args))
                class Thread:
                    def __init__(self, *, target, daemon):
                        self.target = target
                    def start(self):
                        jobs.append(self.target)
                model = sensor_diagnostics.SensorDiagnostics(decode_sensors(self.raw), "observed")
                with patch.object(gui.threading, "Thread", Thread), patch.object(
                        sensor_diagnostics, "read_sensor_diagnostics", return_value=model) as reader:
                    app._read_sensor_diagnostics()
                    self.assertTrue(app.busy)
                    if outcome == "close_window":
                        app.sensor_window.winfo_exists.return_value = False
                    elif outcome == "new_window":
                        app._sensor_view_generation += 1
                    elif outcome == "close_app":
                        app._post = Mock(side_effect=RuntimeError("destroyed"))
                    else:
                        reader.side_effect = OSError("unplugged")
                    jobs[0]()
                for fn, args in callbacks:
                    fn(*args)
                app._apply_sensor_diagnostics.assert_not_called()
                if outcome != "close_app":
                    self.assertFalse(app.busy)
                if outcome == "failure":
                    app.sensor_message.set.assert_called_with("Sensor inspection failed: unplugged")
                    app.sensor_tree.delete.assert_called_with("old")
                    app.sensor_details.insert.assert_called_with("end", "Sensor inspection failed: unplugged")

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

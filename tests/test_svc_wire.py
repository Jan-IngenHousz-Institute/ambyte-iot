# SPDX-License-Identifier: GPL-3.0-or-later
"""Production framing/codec/editor filter under ASan and UBSan."""
import ast
import base64
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile
import unittest
import zlib
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def frame(body):
    if isinstance(body, str):
        body = body.encode()
    return body + b"*%08x\n" % zlib.crc32(body)


class ServiceWireTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "wire"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I" + str(ROOT / "components/usb_service/include"),
            str(ROOT / "components/usb_service/svc_wire.c"),
            str(ROOT / "tests/svc_host/wire_driver.c"), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_wire(self, mode, data, *extra):
        return subprocess.run([str(self.binary), mode, data.hex(), *map(str, extra)],
                              check=True, text=True, capture_output=True).stdout

    def test_crc_vectors(self):
        rng = random.Random(163)
        for data in (b"", b"a", b"abc", b"123456789", bytes(range(256)),
                     *(rng.randbytes(n) for n in range(0, 600, 31))):
            self.assertEqual(self.run_wire("crc", data).strip(), f"{zlib.crc32(data):08x}")

    def test_verify_before_dispatch_and_recovery(self):
        valid = frame("00000001 svc.version")
        for i in range(len(valid) - 1):
            altered = bytearray(valid)
            altered[i] ^= 1
            result = self.run_wire("rx", bytes(altered) + valid)
            self.assertEqual(result.count("F "), 1, (i, result))
            self.assertIn("F 00000001 svc.version\n", result)
        self.assertEqual(self.run_wire("rx", valid * 20).count("F "), 20)

    def test_exact_chunk_boundary_and_overflow(self):
        request = frame(b"ffffffff p ffffffffffffffff ffffffff ffffffff " + b"A" * 192)
        self.assertEqual(len(request), 248)
        self.assertTrue(self.run_wire("rx", request).startswith("F ffffffff p "))
        for n in (249, 256, 4000):
            result = self.run_wire("rx", b"A" * (n - 1) + b"\n" + frame("00000001 svc.version"))
            self.assertEqual(result, "E line_too_long\nF 00000001 svc.version\n")

    def test_malformed_timeout(self):
        for body in (b"00000000 svc.version", b"0000000G svc.version"):
            self.assertEqual(self.run_wire("rx", frame(body)), "E bad_frame\n")
        for data in (b"00000001\0garbage\n", b"00000001 svc.version\r\n", b"\n"):
            self.assertEqual(self.run_wire("rx", data), "E bad_frame\n")
        self.assertEqual(self.run_wire("rx", b"00000001 svc.", 2000), "E frame_timeout\n")

    def test_strict_base64(self):
        rng = random.Random(163)
        for n in range(1, 145):
            data = rng.randbytes(n)
            self.assertEqual(self.run_wire("b64", base64.b64encode(data)).strip(), f"{n} {data.hex()}")
        for value in (b"", b"!!!", b"YQ", b"YQ=", b"YQ===", b"YR==", b"YWJ=", b"YQ==AAAA", b"Y Q==", b"===="):
            self.assertTrue(self.run_wire("b64", value).startswith("-1 "), value)
        self.assertTrue(self.run_wire("b64", base64.b64encode(b"a" * 145)).startswith("-2 "))

    def test_filter_passthrough_and_late_echo(self):
        source = (ROOT / "components/CLI/CLI.c").read_text()
        names = re.findall(r'\.command\s*=\s*"([^"]+)"', source)
        self.assertGreater(len(names), 30)
        for name in names:
            self.assertIsNone(re.fullmatch("[0-9a-f]{8}", name), name)
        lines = [name.encode() + b"\n" for name in names]
        lines += [b'cfg set device_name "deadbeef example"\n',
                  b'schedule commit ' + b'a' * 64 + b' id version fw\n',
                  b'cfg get mqtt_client_id\n', b'ping\n',
                  b'\x1b[A', b'\x1b[B', b'\x1b[C', b'\x1b[D', b'\x1b[5n', b'\x1b[0n', b'\x1b[12;80R']
        for line in lines:
            self.assertEqual(self.run_wire("filter", line).strip(), line.hex(), line)
        trace = self.run_wire("trace", b"abcdefg").splitlines()
        self.assertEqual(trace[:6], [""] * 6)
        self.assertEqual(trace[6], b"abcdefg".hex())
        self.assertEqual(self.run_wire("filter", b"deadbeef").strip(), "")
        request = frame("00000001 svc.wifi.join 0000000000000001 YQ== c2VjcmV0")
        self.assertEqual(self.run_wire("filter", request + b"identity\n").strip(), b"identity\n".hex())
        self.assertEqual(self.run_wire("filter", b"\x1b[0n" + request).strip(), b"\x1b[0n".hex())

    def test_filter_never_exposes_credentials_after_editor_input(self):
        request = frame("00000001 svc.wifi.join 0000000000000001 YQ== c2VudGluZWwtcGFzc3dvcmQ=")
        for prefix in (b"\x03", b"ab\x7f\x7f", b"x", b" ", b"\x15", b"identity\n", b"\x1b[0n", b"\x1b", b"\x1b[", b"a", b"deadbeef ", b"x deadbeef stray ", b"9" * 40):
            with self.subTest(prefix=prefix):
                result = bytes.fromhex(self.run_wire("filter", prefix + request + b"identity\n").strip())
                expected_prefix = b"" if prefix == b"deadbeef " else prefix
                self.assertEqual(result, expected_prefix + b"identity\n")

    def test_filter_nested_window_and_damaged_credential_crc(self):
        body = b"00000001 svc.wifi.join 0000000000000001 " + base64.b64encode(b"s" * 31) + b" " + base64.b64encode(b"password" * 7)
        request = frame(body)
        prefix = b"x deadbeef stray " * 100
        for value in (request, request[:-9] + b"00000000\n"):
            result = bytes.fromhex(self.run_wire("filter", prefix + value + b"identity\n").strip())
            self.assertEqual(result, prefix + b"identity\n")
            self.assertNotIn(base64.b64encode(b"password" * 7), result)

    def test_filter_never_releases_partial_or_oversized_credential_input(self):
        header = b"x00000001 svc.wifi.join 0000000000000001 YQ== "
        for tail in (b"c2VjcmV0\x03\n", b"c2VjcmV0" * 100 + b"\n", b"c2VjcmV0*invalid!\n"):
            result = bytes.fromhex(self.run_wire("filter", header + tail + b"identity\n").strip())
            self.assertEqual(result, b"xidentity\n")

    def test_every_plain_gui_command_template_passes_byte_identical(self):
        from flash_gui.ambyte_serial import AmbyteConsole, ConsoleError
        con = object.__new__(AmbyteConsole)
        captured = []
        staged = 0
        def command(line, *args, **kwargs):
            nonlocal staged
            captured.append(line)
            if line == "schedule begin": staged = 0; return "schedule begin: ready"
            if line.startswith("schedule put "):
                staged += len(base64.b64decode(line.split()[-1]))
                return f"schedule put: {staged} bytes"
            if line.startswith("schedule commit "): return "schedule commit queued: fixture"
            if line.startswith("schedule install "): return "schedule install queued: fixture"
            return "(reboot to apply)"
        con.command = command
        con._write_line = lambda line: captured.append(line)
        operations = [con.identity, con.status, con.rtc_read, lambda: con.rtc_set(1790000000),
                      con.schedule_release, con.schedule_status_workbook, con._schedule_abort_quietly,
                      lambda: con.schedule_install("https://example.test/schedule.yaml", "a" * 64, "deadbeef", "a3", "test"),
                      lambda: con.schedule_push(bytes(range(256)) * 2, "0123456789abcdef" * 4, "deadbeef", "a3", "test")]
        for key in ("device_name", "timezone", "mqtt_client_id"):
            operations += [lambda k=key: con.cfg_get(k), lambda k=key: con.cfg_set(k, "deadbeef example"),
                           lambda k=key: con.cfg_set(k, "12345678"),
                           lambda k=key: con.cfg_set(k, "deadbeef svc.weekly"),
                           lambda k=key: con.cfg_set(k, "deadbeef svc.wifi.join literal")]
        for operation in operations:
            try: operation()
            except ConsoleError: pass  # only response parsing fails; emitted command is captured
        with patch("flash_gui.ambyte_serial.time.sleep"):
            con.reboot()
        # Inventory every actual console emission site. A new dynamic template
        # fails here until a real serializer exercise above covers its shape.
        for source in sorted((ROOT / "flash_gui").glob("*.py")):
            if source.name.startswith("service_"): continue  # framed clients bypass the editor
            for call in ast.walk(ast.parse(source.read_text())):
                if not (isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
                        and call.func.attr in {"command", "_write_line"} and call.args): continue
                arg = call.args[0]
                if isinstance(arg, ast.Constant) and isinstance(arg.value, str): captured.append(arg.value)
                elif isinstance(arg, ast.JoinedStr):
                    pattern = "".join(re.escape(v.value) if isinstance(v, ast.Constant) else ".*" for v in arg.values)
                    self.assertTrue(any(re.fullmatch(pattern, line) for line in captured), (source, ast.unparse(arg)))
                elif isinstance(arg, ast.Name) and arg.id == "cmd":
                    self.assertEqual(source.name, "ambyte_serial.py")
                elif isinstance(arg, ast.BinOp):
                    self.assertTrue(ast.unparse(arg).startswith("'schedule commit ' +"))
                    self.assertTrue(any(line.startswith("schedule commit ") for line in captured))
                else: self.fail(f"Unexercised console template {source}: {ast.unparse(arg)}")
        # fleet_deploy sends MQTT payloads and has no serial console writer.
        for source in sorted((ROOT / "tools/fleet_deploy").glob("*.py")):
            for call in ast.walk(ast.parse(source.read_text())):
                if isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute):
                    self.assertNotIn(call.func.attr, {"command", "_write_line", "Serial"}, source)
        self.assertGreater(len(captured), 30)
        for ending in ("\n", "\r\n"):
            stream = "".join(line + ending for line in captured).encode()
            self.assertEqual(bytes.fromhex(self.run_wire("filter", stream).strip()), stream)

    def test_utf8(self):
        for data in (b"abc", "🌱é网络".encode(), b"\xf4\x8f\xbf\xbf"):
            self.assertEqual(self.run_wire("utf8", data).strip(), "1")
        for data in (b"\0", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe2\x82", b"\xff"):
            self.assertEqual(self.run_wire("utf8", data).strip(), "0")


if __name__ == "__main__":
    unittest.main()

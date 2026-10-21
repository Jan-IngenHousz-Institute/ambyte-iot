"""Pinned SDK USB ISR/driver/VFS with service hooks and the actual SD tee."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib
from pinned_idf import pinned_idf

ROOT = Path(__file__).resolve().parents[1]


class ServicePinnedSdkTest(unittest.TestCase):
    def test_real_driver_vfs_stdout_sd_tee_uart_rings_and_pipelined_overrun(self):
        IDF = pinned_idf()
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            base = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-function", "-g", "-O1", "-fno-builtin",
                    "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            includes = [ROOT / "tests/svc_host/sdk_include", ROOT / "tests/svc_host/include",
                        ROOT / "components/usb_service/include"]
            sources = [IDF / "components/esp_driver_usb_serial_jtag/src" / name for name in
                       ("usb_serial_jtag.c", "usb_serial_jtag_vfs.c")]
            sources += [ROOT / "components/usb_service" / name for name in
                        ("svc_usb.c", "svc_wire.c", "svc_store.c", "svc_protocol.c", "svc_engine.c", "svc_session.c")]
            sources += [ROOT / "tests/svc_host/sdk_driver.c"]
            objects = []
            for index, src in enumerate(sources):
                obj = build / f"{index}.o"
                p = subprocess.run([*base, *(f"-I{x}" for x in includes), "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
                self.assertEqual(p.returncode, 0, p.stderr)
                objects.append(obj)
            sd_obj = build / "sd.o"
            sd_includes = [ROOT / "tests/sd_shim/stubs", ROOT / "components/sd_logger", ROOT / "components/sd_card",
                           ROOT / "components/event_log/include"]
            p = subprocess.run([*base, *(f"-I{x}" for x in sd_includes), "-c", str(ROOT / "tests/svc_host/sdk_sd_bridge.c"),
                                "-o", str(sd_obj)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            symbols = ("usb_serial_jtag_driver_install", "xRingbufferCreate", "xRingbufferSendFromISR",
                       "xRingbufferReceiveUpTo", "vRingbufferReturnItem", "usb_serial_jtag_write_bytes")
            binary = build / "sdk"
            p = subprocess.run([*base, *map(str, objects), str(sd_obj), "-Wl,--gc-sections",
                                *(f"-Wl,--wrap={name}" for name in symbols), "-pthread", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            p = subprocess.run([str(binary), str(build / "svc")], capture_output=True, timeout=10)
            self.assertEqual(p.returncode, 0, p.stderr.decode())
            self.assertIn(b"PINNED_SDK_OK", p.stderr)
            replies = []
            for line in p.stdout.splitlines(keepends=True):
                self.assertTrue(line.endswith(b"\n"))
                body, crc = line[:-1].rsplit(b"*", 1)
                self.assertEqual(int(crc, 16), zlib.crc32(body))
                replies.append((int(body[:8], 16), json.loads(body[9:])))
            self.assertEqual(len(replies), 106)  # 102 normal frames + ready/overrun/version/exit
            self.assertEqual([item[0] for item in replies[:102]], list(range(102)))
            self.assertEqual(replies[103][1]["reason"], "rx_overrun")
            self.assertEqual([item[0] for item in replies[104:]], [99, 100])
            self.assertEqual((build / "svc" / "stage.bin").exists(), False)


if __name__ == "__main__": unittest.main()

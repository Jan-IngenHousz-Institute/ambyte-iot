"""Replacement amendment A2/A7 host tests: the HIL tools have no default bench board,
D-2 binds the configured device_id/device_name (not the MAC), and wifi_provision never
writes the passphrase - even when the console redraws the echo with ANSI escapes."""
from __future__ import annotations

import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "evq_hil"))
import bench  # noqa: E402
import wifi_provision as W  # noqa: E402


class BenchIdentity(unittest.TestCase):
    def test_no_default_board(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            for f in (bench.mac, bench.port, bench.client, bench.experiment, bench.device_id, bench.device_name):
                with self.assertRaises(SystemExit):
                    f()

    def test_replacement_identity(self):
        env = {"AMBYTE_BENCH_MAC": "e8:f6:0a:b1:1f:34", "AMBYTE_BENCH_DEVICE_ID": "03:25:07:04",
               "AMBYTE_BENCH_DEVICE_NAME": "AMBYTE_E8:F6:0A:B1:1F:34",
               "AMBYTE_BENCH_EXPERIMENT": "b3c8d242-dbb3-4520-9a5c-664e364ae6bd"}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(bench.mac(), "E8:F6:0A:B1:1F:34")
            self.assertEqual(bench.client(), "ambyte_e8:f6:0a:b1:1f:34")
            self.assertTrue(bench.port().endswith("E8:F6:0A:B1:1F:34-if00"))
            self.assertEqual(bench.device_id(), "03:25:07:04")
            self.assertNotEqual(bench.device_id(), bench.mac())

    def test_d2_checks_configured_device_id_not_mac(self):
        import reconcile_warehouse as rw
        env = {"AMBYTE_BENCH_MAC": "E8:F6:0A:B1:1F:34", "AMBYTE_BENCH_DEVICE_ID": "03:25:07:04",
               "AMBYTE_BENCH_DEVICE_NAME": "AMBYTE_E8:F6:0A:B1:1F:34",
               "AMBYTE_BENCH_EXPERIMENT": "b3c8d242-dbb3-4520-9a5c-664e364ae6bd"}
        exp = {"run": "s2-t", "k": 100000, "id": 5, "start_ms": 1790000000000, "end_ms": 1790000000000}
        import hilpay, json, datetime
        data = json.loads(hilpay.payload("s2-t", 100000, 16))
        iso = datetime.datetime.fromtimestamp(1790000000, datetime.UTC).isoformat().replace("+00:00", "Z")
        base = {"experiment_id": env["AMBYTE_BENCH_EXPERIMENT"],
                "sample": {"v": 2, "measure_id": 5, "startTicks_UTC": exp["start_ms"], "endTicks_UTC": exp["end_ms"],
                           "channel": None, "device": "evq_hil", "cmd_raw": "evq_hil s2-t", "tag": "MEASUREMENT",
                           "metadata": None, "data": data},
                "env": {"timestamp": iso, "device_id": "03:25:07:04", "device_name": "AMBYTE_E8:F6:0A:B1:1F:34"}}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual([b for b in rw.check_synthetic(base, exp, 16, None, {}) if "device" in b], [])
            wrong = json.loads(json.dumps(base))
            wrong["env"]["device_id"] = "E8:F6:0A:B1:1F:34"     # the MAC is NOT the configured id here
            self.assertTrue(any(b.startswith("device_id") for b in rw.check_synthetic(wrong, exp, 16, None, {})))


class WifiProvision(unittest.TestCase):
    PW = "s3cretPassphrase9"

    def test_redacts_plain_and_ansi_split_echo(self):
        pw = self.PW
        esc = "".join(ch + "\x1b[0K" for ch in pw)                     # redraw between every character
        raw = (f'ambyte> wifi_join "lab" "{pw}"\r\n' + f"\x1b[2K\rambyte> wifi_join \"lab\" \"{esc}\"\r\n"
               + 'wifi_join "lab": ESP_OK\r\n').encode()
        safe, n = W.redact(raw, pw)
        self.assertNotIn(pw.encode(), safe)
        self.assertEqual(n, 2)
        self.assertIn(b'wifi_join "lab": ESP_OK', safe)

    def test_redacts_ssid_too(self):
        raw = f'ambyte> wifi_join "BenchNet-7" "{self.PW}"\r\nwifi_join "BenchNet-7": ESP_OK\r\n'.encode()
        safe, n = W.redact(raw, self.PW, "BenchNet-7")
        self.assertNotIn(b"BenchNet-7", safe)
        self.assertNotIn(self.PW.encode(), safe)
        self.assertIn(b'wifi_join "<redacted-ssid>": ESP_OK', safe)

    def test_creds_file_must_be_private(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c.env"
            p.write_text(f"SSID=lab\nPASS={self.PW}\n")
            os.chmod(p, 0o644)
            with self.assertRaises(SystemExit):
                W.load_creds(p)
            os.chmod(p, 0o600)
            self.assertEqual(W.load_creds(p), ("lab", self.PW))
            self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600)

    def test_refuses_console_hostile_chars(self):
        for bad in ('pa"ss1234', "pa\\ss1234", "pa'ss1234", "pass\n1234"):
            with self.assertRaises(SystemExit):
                W.check("lab", bad)
        W.check("lab", self.PW)


if __name__ == "__main__":
    unittest.main()

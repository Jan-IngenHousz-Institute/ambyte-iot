# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""AWS IoT Core Device Location codec against the contract verified on DEV.

The openJII policy work (2026-11-13) verified by round trip: the topic id is
the thing name with every byte outside [A-Za-z0-9-] replaced by '-' (other ids
are dropped silently); the request schema is strict (Timestamp a STRING,
unknown keys 400); Wi-Fi and Ip must not be combined; the device must send its
own PUBLIC IP; replies are GeoJSON [lon, lat] with properties.messageId.
"""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Shape verbatim from the DEV round trip; position, place and ids synthetic
# (the real answer located the bench site, which stays out of this public repo).
ACCEPTED = ('{"coordinates":[4.8900,52.3700],"type":"Point","properties":{"horizontalAccuracy":5000,'
            '"horizontalConfidenceLevel":0.67,"messageId":"tz-42","timestamp":"2026-11-13T08:50:26.845Z",'
            '"country":"The Netherlands","state":"North Holland","city":"Amsterdam","postalCode":"1000"}}')
ULURU_EGRESS = ('{"coordinates":[138.6471,-34.8545],"type":"Point","properties":{"horizontalAccuracy":50000,'
                '"horizontalConfidenceLevel":0.67,"messageId":"tz-7-i"}}')
WIFI_ONLY = ('{"coordinates":[13.376077,52.51823,80.1],"type":"Point","properties":{"verticalAccuracy":45,'
             '"horizontalAccuracy":68,"horizontalConfidenceLevel":0.68,"messageId":"tz-3-w",'
             '"timestamp":"2026-04-07T01:16:39.971767716Z"}}')
REJECTED = '{"errorCode":404,"errorMessage":"Could not solve position using the given Wifi and Cellular values","messageId":"tz-3-w"}'


class LocationAwsCodecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        packages = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages"
        sdk = packages / "framework-espidf@3.50500.0"
        if not sdk.exists():
            sdk = packages / "framework-espidf"
        cjson = sdk / "components/json/cJSON"
        cls.temp = tempfile.TemporaryDirectory(prefix="loc-aws-")
        cls.binary = Path(cls.temp.name) / "loc-aws"
        subprocess.run([
            os.environ.get("CC", shutil.which("clang") or "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
            f"-I{ROOT / 'tests/host_stubs'}", f"-I{ROOT / 'components/domain/include'}", f"-I{cjson}",
            f"-I{ROOT / 'components/command_router/include'}",
            str(ROOT / "tests/location_aws_host.c"), str(ROOT / "components/command_router/location_aws_codec.c"),
            str(cjson / "cJSON.c"), "-lm", "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_op(self, *args):
        run = subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        return run.stdout.strip(), run.stderr.strip()

    def locids(self, names):
        stdin = "".join(n.encode("utf-8").hex() + "\n" for n in names)
        run = subprocess.run([str(self.binary), "locids"], input=stdin, capture_output=True,
                             text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        return [None if line == "!" else line for line in run.stdout.splitlines()]

    def test_location_id_shared_vectors(self):
        # VERBATIM from open-jii #2430,
        # apps/backend/src/common/modules/aws/services/iot/device-location-id.spec.ts
        # ("Shared with the Ambyte firmware: both sides must produce these ids
        # byte for byte"). Every byte outside [A-Za-z0-9], '-' included, becomes
        # '-' + two uppercase hex digits; over 128 chars the client stays inert.
        vectors = [
            ["ambyte_28:37:2F:FF:E7:04", "ambyte-5F28-3A37-3A2F-3AFF-3AE7-3A04"],
            ["AMBYTE_E8:F6:0A:B1:1D:D4", "AMBYTE-5FE8-3AF6-3A0A-3AB1-3A1D-3AD4"],
            ["AMBYTE28:37:2F:FF:FC:24", "AMBYTE28-3A37-3A2F-3AFF-3AFC-3A24"],
            ["mobile_614277f4-9fc5", "mobile-5F614277f4-2D9fc5"],
            ["a-b", "a-2Db"],
            ["a_b", "a-5Fb"],
            ["a:b", "a-3Ab"],
            ["-", "-2D"],
            ["caf\u00e9", "caf-C3-A9"],
            ["", None],
            ["a" * 128, "a" * 128],
            ["a" * 129, None],
            ["a" * 125 + "_", "a" * 125 + "-5F"],
            ["a" * 126 + "_", None],
        ]
        self.assertEqual(len(vectors), 14)
        self.assertEqual(self.locids([v[0] for v in vectors]), [v[1] for v in vectors])

    def test_location_id_is_injective_and_aws_safe(self):
        # The platform spec's property checks, same seeded generator.
        alphabet = ["a", "Z", "0", "9", "-", "_", ":", ".", "\u00e9"]

        def seeded(seed):
            state = seed
            def rnd():
                nonlocal state
                state = (state * 1103515245 + 12345) % 2 ** 31
                return state / 2 ** 31
            return rnd

        def names(seed, count):
            rnd = seeded(seed)
            out = []
            for _ in range(count):
                length = 1 + int(rnd() * 12)
                out.append("".join(alphabet[int(rnd() * len(alphabet))] for _ in range(length)))
            return out

        sample = names(42, 5000)
        owners = {}
        for name, loc_id in zip(sample, self.locids(sample)):
            self.assertIsNotNone(loc_id)
            self.assertRegex(loc_id, r"^[A-Za-z0-9-]+$")
            self.assertLessEqual(len(loc_id), 128)
            self.assertEqual(owners.setdefault(loc_id, name), name)
        trio = self.locids(["generic_edge_box", "generic_edge-box", "generic_edge:box"])
        self.assertEqual(len(set(trio)), 3)
        mac, dashed = self.locids(["ambyte_28:37:2F:FF:E7:04", "ambyte_28-37-2F-FF-E7-04"])
        self.assertNotEqual(mac, dashed)

    def test_wifi_request_matches_the_strict_schema(self):
        body = json.loads(self.run_op("wifi")[0])
        self.assertEqual(set(body), {"Timestamp", "MessageId", "WiFiAccessPoints"})
        self.assertEqual(body["Timestamp"], "1794559977")            # a STRING
        self.assertEqual(body["MessageId"], "tz-1-w")
        self.assertEqual(body["WiFiAccessPoints"], [
            {"MacAddress": "00:00:5E:00:53:01", "Rss": -61},
            {"MacAddress": "00:00:5E:00:53:03", "Rss": -50},
            {"MacAddress": "00:00:5E:00:53:02", "Rss": -75},
        ][::1] and sorted(body["WiFiAccessPoints"], key=lambda a: -a["Rss"]))
        self.assertEqual([a["Rss"] for a in body["WiFiAccessPoints"]], [-50, -61, -75])

    def test_ip_request_never_mixes_wifi(self):
        body = json.loads(self.run_op("ip", "9.9.9.9")[0])
        self.assertEqual(body, {"Timestamp": "1794559977", "MessageId": "tz-2-i",
                                "Ip": {"IpAddress": "9.9.9.9"}})

    def test_public_ip_validation(self):
        for text, want in [
            ("9.9.9.9\n", "9.9.9.9"),
            ("8.8.4.4\r\n", "8.8.4.4"),
            ("2a02:a45f:1::5\n", "2a02:a45f:1::5"),
            ("10.0.0.7", "!"), ("192.168.1.20", "!"), ("172.20.1.1", "!"), ("100.72.1.9", "!"),
            ("127.0.0.1", "!"), ("169.254.3.4", "!"), ("224.0.0.1", "!"), ("0.0.0.0", "!"),
            ("fe80::1", "!"), ("::1", "!"), ("fd00::1", "!"),
            ("1.2.3", "!"), ("01.2.3.4", "!"), ("256.1.1.1", "!"), ("<html>", "!"), ("", "!"),
            ("1.2.3.4 extra", "!"),
        ]:
            with self.subTest(text):
                self.assertEqual(self.run_op("publicip", text)[0], want)

    def test_replies(self):
        out, acc = self.run_op("reply", "accepted", ACCEPTED)
        r = json.loads(out)
        self.assertTrue(r["accepted"])
        self.assertAlmostEqual(r["lat"], 52.3700)      # [lon, lat] order honoured
        self.assertAlmostEqual(r["lon"], 4.8900)
        self.assertEqual((acc, r["message_id"]), ("5000.0", "tz-42"))
        r = json.loads(self.run_op("reply", "accepted", ULURU_EGRESS)[0])
        self.assertAlmostEqual(r["lat"], -34.8545)
        r = json.loads(self.run_op("reply", "accepted", WIFI_ONLY)[0])
        self.assertEqual(r["message_id"], "tz-3-w")
        r = json.loads(self.run_op("reply", "rejected", REJECTED)[0])
        self.assertEqual((r["accepted"], r["error_code"], r["message_id"]), (False, 404, "tz-3-w"))
        no_acc = json.loads(self.run_op("reply", "accepted",
                                        '{"coordinates":[1,2],"type":"Point","properties":{}}')[0])
        self.assertIsNone(no_acc["accuracy_m"])        # the gate then rejects it
        for bad in ("{", '{"coordinates":[200,1]}', '{"coordinates":[1]}', '{"type":"Point"}'):
            self.assertEqual(self.run_op("reply", "accepted", bad)[0], "!", bad)


if __name__ == "__main__":
    unittest.main()

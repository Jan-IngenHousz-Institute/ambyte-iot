"""Run the production identity handler, runner snapshot and release provider.

SDK boundaries are replaced; no production function is copied or extracted.
The runner test includes its whole real translation unit to seed private state.
Unreachable firmware paths are removed by section GC, not preprocessor forks.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/identity_host"
CAPABILITIES = {
    "identity", "cfg.get", "cfg.set", "reboot", "schedule.status",
    "schedule.release", "schedule.begin", "schedule.put", "schedule.commit", "schedule.abort",
}
WORKBOOK = "11111111-2222-4333-8444-555555555555"


class IdentityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="identity-host-")
        cls.out = Path(cls.temp.name)
        includes = [HOST / "include", ROOT / "tests/host_stubs"]
        includes += list((ROOT / "components").glob("*/include"))
        includes += [ROOT / "components/sched_runner"]
        # sched_runner includes its generated default even though these tests
        # never start a task or compile the embedded fixture.
        (cls.out / "default_yaml_embed.h").write_text(
            'static const char default_yaml_start[] = "";\n'
            'static const unsigned default_yaml_size = 0;\n')
        includes.append(cls.out)
        common = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            *(f"-I{p}" for p in includes),
        ]
        builds = {
            "handler": [HOST / "handler.c", ROOT / "components/CLI/cli_identity.c",
                        "-Wl,--wrap=fwrite", "-pthread"],
            "runner": [HOST / "runner.c", *sorted((ROOT / "components/sched_spec").glob("*.c")),
                       ROOT / "components/time_sync/time_sync.c", "-Wl,--wrap=sched_pool_str", "-lm"],
            "provider": [HOST / "provider.c", ROOT / "components/script_update/script_update.c",
                         "-Wl,--wrap=fopen", "-lcrypto"],
        }
        for name, sources in builds.items():
            cmd = [*common, *(str(p) for p in sources), "-o", str(cls.out / name)]
            result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, scenario, *args):
        run = subprocess.run([self.out / "handler", scenario, *args], capture_output=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertIn(b"IDENTITY_HOST_OK", run.stderr)
        return run.stdout

    def response(self, scenario, *args):
        raw = self.invoke(scenario, *args)
        physical = raw.split(b"\n")
        objects = [line.rstrip(b"\r") for line in physical if line.startswith(b"{")]
        self.assertEqual(len(objects), 1, raw)
        line = objects[0]
        self.assertTrue(line.endswith(b"}"))
        self.assertTrue(all(c >= 32 for c in line))
        result = json.loads(line.decode("utf-8"))
        self.assertEqual(set(result), {"mac", "thing_name", "fw", "contract", "schedule", "capabilities"})
        self.assertIs(type(result["contract"]), int)
        self.assertEqual(result["contract"], 1)
        self.assertEqual(set(result["schedule"]), {"source", "sha256", "workbookVersionId", "script_version"})
        self.assertEqual(set(result["capabilities"]), CAPABILITIES)
        self.assertEqual(len(result["capabilities"]), len(CAPABILITIES))
        for secret in (b"password", b"PRIVATE KEY", b"CERTIFICATE", b"secret_sentinel"):
            self.assertNotIn(secret, raw)
        return result

    def test_compiled_effective_identity_and_loaded_release(self):
        obj = self.response("installed")
        self.assertEqual(obj["mac"], "02:00:00:00:00:01")
        self.assertEqual(obj["thing_name"], "Field_MixedCase_7")
        self.assertEqual(obj["fw"], "2.6.0-a2")
        self.assertEqual(obj["schedule"], dict(source="INSTALLED", sha256="a" * 64,
                        workbookVersionId=WORKBOOK, script_version="schedule-v1.2.3"))
        self.assertEqual(self.response("template")["thing_name"], "ambyte_02:00:00:00:00:01")

    def test_sources_and_unavailable_snapshots(self):
        self.assertEqual(self.response("none")["schedule"], dict(source="NONE", sha256=None,
                         workbookVersionId=None, script_version=None))
        for name, source in (("default", "EMBEDDED_DEFAULT"), ("fallback", "EMBEDDED_FALLBACK")):
            with self.subTest(name=name):
                obj = self.response(name)["schedule"]
                self.assertEqual(obj, dict(source=source, sha256="a" * 64,
                                 workbookVersionId=WORKBOOK, script_version=None))
        for scenario in ("starting", "reload", "reload_same_sha", "unknown_source"):
            with self.subTest(scenario=scenario):
                self.assertEqual(self.response(scenario)["schedule"], dict.fromkeys(
                    ("source", "sha256", "workbookVersionId", "script_version")))

    def test_release_trust_and_missing_fields(self):
        for scenario in ("stale", "wrong_file", "missing_file", "empty_release", "malformed_sha", "short_sha"):
            with self.subTest(scenario=scenario):
                obj = self.response(scenario)["schedule"]
                self.assertIsNone(obj["script_version"])
                self.assertEqual(obj["workbookVersionId"], WORKBOOK)
        for scenario in ("malformed_sha", "short_sha"):
            self.assertIsNone(self.response(scenario)["schedule"]["sha256"])
        for scenario in ("no_workbook", "empty_workbook", "dash_workbook"):
            self.assertIsNone(self.response(scenario)["schedule"]["workbookVersionId"])
        obj = self.response("missing_device")
        self.assertIsNone(obj["mac"])
        self.assertIsNone(obj["thing_name"])
        self.assertIsNone(obj["fw"])

    def test_json_escaping_and_full_capacity(self):
        obj = self.response("escaping")
        self.assertEqual(obj["thing_name"], 'Field_"\\\n\r\t\x01é')
        self.assertEqual(obj["fw"], 'fw_"\\\n\r\t\x1f')
        self.assertEqual(obj["schedule"]["workbookVersionId"], 'wb_"\\\n\r\t\x02')
        self.assertEqual(obj["schedule"]["script_version"], 'rel_"\\\n\r\t\x03')
        obj = self.response("maximum_escaping")
        self.assertEqual(obj["thing_name"], "\x01" * 63)
        self.assertEqual(obj["fw"], "\x02" * 31)
        self.assertEqual(obj["schedule"]["workbookVersionId"], "\x03" * 63)
        self.assertEqual(obj["schedule"]["script_version"], "\x04" * 31)

    def test_prefix_separation_and_bad_arguments(self):
        for prefix in ("prompt", "log_tail"):
            self.response(prefix)
        self.assertEqual(self.invoke("usage"), b"Usage: identity\r\n")

    def test_invalid_utf8_fields_are_null_and_discovery_remains_json(self):
        invalid = [
            b"x\xff\xfey", b"\x80", b"\xbf", b"\xc0\x80", b"\xc1\xbf",
            b"\xe0\x80\x80", b"\xf0\x80\x80\x80", b"\xed\xa0\x80",
            b"\xed\xbf\xbf", b"\xf4\x90\x80\x80", b"\xf5\x80\x80\x80",
            b"\xc2", b"\xe2\x82", b"\xf0\x9f\x92", b"\xe2(\xa1",
            b"valid\xc3\xa9\x80tail", b"\xf8\x88\x80\x80\x80",
            b"a" * 30 + b"\xc2", b"a" * 28 + b"\xf0\x9f\x92",
        ]
        valid = [
            b"plain\x7f", "é€🙂".encode(), b"\xc2\x80", b"\xdf\xbf",
            b"\xe0\xa0\x80", b"\xed\x9f\xbf", b"\xef\xbf\xbf",
            b"\xf0\x90\x80\x80", b"\xf4\x8f\xbf\xbf",
        ]
        rng = random.Random(0xA2)
        vectors = invalid + valid + [bytes([byte]) for byte in range(1, 256)] + [
            bytes(rng.randrange(1, 256) for _ in range(rng.randrange(1, 32)))
            for _ in range(100)
        ]
        for data in vectors:
            with self.subTest(hex=data.hex()):
                try:
                    expected = data.decode("utf-8")
                except UnicodeDecodeError:
                    expected = None
                obj = self.response("utf8_bytes", data.hex())
                self.assertEqual(obj["thing_name"], expected)
                self.assertEqual(obj["fw"], expected)
                self.assertEqual(obj["schedule"]["workbookVersionId"],
                                 None if expected == "-" else expected)
                self.assertEqual(obj["schedule"]["script_version"], expected)
                self.assertEqual(obj["schedule"]["sha256"], "a" * 64)

    def test_real_handler_blocks_competing_writer_until_record_complete(self):
        output = self.invoke("interleaving")
        lines = [line.rstrip(b"\r") for line in output.split(b"\n") if line.rstrip(b"\r")]
        objects = [line for line in lines if line.startswith(b"{")]
        logs = [line for line in lines if line.startswith(b"I (host) unrelated log ")]
        self.assertEqual(len(objects), 500)
        self.assertGreaterEqual(len(logs), 100)
        self.assertEqual(len(lines), len(objects) + len(logs))
        for line in objects:
            self.assertTrue(line.endswith(b"}"))
            self.assertEqual(json.loads(line)["contract"], 1)

    def test_real_runner_atomic_snapshot_and_stopped_loaded_program(self):
        run = subprocess.run([self.out / "runner", HOST / "bench.yaml"],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("IDENTITY_RUNNER_OK", run.stdout)

    def provider(self, data=b"original schedule\n", **changes):
        schedule = self.out / "schedule.yaml"
        schedule.write_bytes(data)
        env = {k: v for k, v in os.environ.items() if not k.startswith("A2_")}
        digest = hashlib.sha256(data).hexdigest()
        env.update(A2_SCHEDULE_FILE=str(schedule), A2_STORED_SHA=digest,
                   A2_RELEASE_VERSION="schedule-v1.2.3", A2_BUILT_FW="2.4.0",
                   A2_INSTALLED_FW="2.5.0")
        env.update(changes)
        run = subprocess.run([self.out / "provider"], env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return dict(line.split("=", 1) for line in run.stdout.splitlines()), digest

    def test_real_provider_read_only_digest_trust_survives_firmware_change(self):
        obj, digest = self.provider()
        self.assertEqual(obj, dict(rc="0", sha=digest, version="schedule-v1.2.3",
                         built="2.4.0", installed="2.5.0", verified="1"))
        # Real provider has no current-firmware dependency; installation
        # firmware is historical, not a condition for verified metadata.
        obj, _ = self.provider(A2_INSTALLED_FW="1.0.0")
        self.assertEqual(obj["verified"], "1")
        self.assertEqual(obj["installed"], "1.0.0")

    def test_real_provider_rejects_stale_or_incomplete_release_metadata(self):
        for changes in ({"A2_STORED_SHA": "b" * 64}, {"A2_RELEASE_VERSION": ""},
                        {"A2_BUILT_FW": ""}, {"A2_NVS_FAIL": "1"}):
            with self.subTest(changes=changes):
                obj, digest = self.provider(**changes)
                self.assertEqual(obj["rc"], "0")
                self.assertEqual(obj["sha"], digest)
                self.assertEqual(obj["verified"], "0")
                self.assertEqual(obj["version"], "")
        obj, _ = self.provider(A2_SCHEDULE_FILE=str(self.out / "missing-during-rename"))
        self.assertNotEqual(obj["rc"], "0")
        self.assertEqual(obj["verified"], "0")


if __name__ == "__main__":
    unittest.main()

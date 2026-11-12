# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Remote AMBIT release fetch into /sdcard/ambit_fw/<ver>/ and the manifest gate.

Host-compiles the production ambit_fw_fetch.c, ambit_fw_manifest.c and
ambit_flash_preflight.c through the SD media shim (tests/sd_shim): injected
EIO/short writes, read-back bit flips and simulated CPU resets mid-write, with
every SD op checked to run inside the io gate. The HTTP transport is a scripted
URL -> file table (tests/ambit_host/fetch_driver.c).

The property under test: whatever the network, the card or a reset does, the
folder is either refused by the flash preflight (and by boot sync's target scan)
or holds exactly the bytes the release manifest names."""

import hashlib
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "ambit_host"))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools" / "fleet_deploy"))

import ambit_lib as L  # noqa: E402
from pinned_idf import pinned_idf  # noqa: E402

ESP_FAIL = -1
ESP_ERR_NOT_FOUND = 0x105
ESP_ERR_INVALID_CRC = 0x109
BASE = "https://example.test/releases/download/v1.4.0/"
MANIFEST_URL = BASE + "manifest.json"
DIR = "./sdcard/ambit_fw/1.4.0"

# The v1.4.0 manifest exactly as ambit-iot published it (2026-10-06).
REAL_V140 = {
    "name": "ambit-iot", "version": "1.4.0", "chip": "esp32c3",
    "flash": [
        {"file": "bootloader.bin", "offset": "0x0", "size": 13248,
         "sha256": "30d47ab1f344cfa69f6b2f718ffa72fc7baea6db047eaabefea162db29e6821c"},
        {"file": "partitions.bin", "offset": "0x8000", "size": 3072,
         "sha256": "148b959cbff1c38aa8e1d5c0ba9d612c54997b945e56a63f41223eef650653a1"},
        {"file": "boot_app0.bin", "offset": "0xe000", "size": 8192,
         "sha256": "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"},
        {"file": "ambit-fw-v1.4.0.bin", "offset": "0x10000", "size": 449712,
         "sha256": "060b917f78a31fad1a99dfd91beb1e8ebb31774f13a3f580f7cd3bdb46132531"},
    ],
    "ota": {"file": "ambit-fw-v1.4.0.bin"},
}

# (SD name, release asset name, offset, size) for the synthetic release.
LAYOUT = [("bootloader.bin", "bootloader.bin", "0x0", 3000),
          ("partitions.bin", "partitions.bin", "0x8000", 3072),
          ("boot_app0.bin", "boot_app0.bin", "0xe000", 8192),
          ("app.bin", "ambit-fw-v1.4.0.bin", "0x10000", 50000)]


def build(out: Path) -> Path:
    idf = pinned_idf()
    out.mkdir(parents=True, exist_ok=True)
    incs = [f"-I{L.SHIM / 'stubs'}", f"-I{L.SHIM}", f"-I{L.HOST}", f"-I{ROOT / 'components/sd_card'}",
            f"-I{ROOT / 'components/event_log/include'}", f"-I{ROOT / 'components/ambit_ota'}",
            f"-I{ROOT / 'components/ambit_flash'}", f"-I{ROOT / 'components/ambit_flash/include'}",
            f"-I{idf / 'components/json/cJSON'}"]
    base = [L.cc(), "-std=gnu11", "-g", "-O1", *L.SAN, "-Wall", "-Wextra", *incs, '-DSD_MOUNT_POINT="./sdcard"']
    objs = []
    shimmed = [ROOT / "components/ambit_ota/ambit_fw_fetch.c", ROOT / "components/ambit_flash/ambit_fw_manifest.c",
               ROOT / "components/ambit_flash/ambit_flash_preflight.c"]
    for s in shimmed:
        o = out / (s.stem + ".o")
        L._run([*base, "-Werror", "-include", str(L.SHIM / "sd_shim.h"), "-c", str(s), "-o", str(o)])
        objs.append(o)
    plain = [L.SHIM / "sd_shim.c", L.SHIM / "sd_host_stubs.c", L.SHIM / "host_rtos.c",
             L.SHIM / "mbedtls_sha256_host.c", ROOT / "components/sd_card/sd_diag_core.c",
             idf / "components/json/cJSON/cJSON.c", L.HOST / "fetch_driver.c"]
    for s in plain:
        o = out / (s.stem + ".o")
        L._run([*base, "-Wno-error", "-c", str(s), "-o", str(o)])
        objs.append(o)
    exe = out / "fetch_driver"
    L._run([L.cc(), *L.SAN, *map(str, objs), "-lpthread", "-o", str(exe)])
    return exe


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


class Release:
    """A synthetic release on 'the server' (host files the driver serves)."""

    def __init__(self, root: Path, seed: int = 1, version: str = "1.4.0"):
        self.dir = root
        self.dir.mkdir(parents=True, exist_ok=True)
        self.blobs = {}
        rnd = __import__("random").Random(seed)
        flash = []
        for sd_name, asset, off, size in LAYOUT:
            data = bytes(rnd.getrandbits(8) for _ in range(size))
            self.blobs[sd_name] = data
            (self.dir / asset).write_bytes(data)
            flash.append({"file": asset, "offset": off, "size": size, "sha256": sha(data)})
        self.manifest = {"name": "ambit-iot", "version": version, "chip": "esp32c3", "flash": flash,
                         "ota": {"file": "ambit-fw-v1.4.0.bin"}}
        self.manifest_bytes = json.dumps(self.manifest, indent=2).encode()
        (self.dir / "manifest.json").write_bytes(self.manifest_bytes)

    def serve(self, dev, **over):
        """Serve the manifest + every asset; over[asset] = (fail_at, clen, status) or a host path."""
        dev.cmd(f"serve {MANIFEST_URL} {self.dir / 'manifest.json'}")
        for _sd, asset, _o, _s in LAYOUT:
            spec = over.get(asset)
            if isinstance(spec, (str, Path)):
                r = dev.cmd(f"serve {BASE}{asset} {spec}")
            else:
                fail_at, clen, status = spec if spec else (-1, -2, 200)
                r = dev.cmd(f"serve {BASE}{asset} {self.dir / asset} {fail_at} {clen} {status}")
            assert r["serve"] == "ok", r


class AmbitFwFetchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.tmp.name)
        cls.exe = build(cls.work / "build")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.state = self.work / f"state-{self._testMethodName}"
        self.dev = L.new_dev(self.exe, self.state)
        self.dev.start()
        self.rel = Release(self.work / f"rel-{self._testMethodName}")
        self.folder = self.state / "sdcard" / "ambit_fw" / "1.4.0"

    def tearDown(self):
        self.dev.finish()

    # ── helpers ──
    def fetch(self, ver="1.4.0", url=MANIFEST_URL):
        return self.dev.cmd(f"fetch {ver} {url}")

    def files(self):
        return sorted(os.listdir(self.folder)) if self.folder.exists() else []

    def assert_folder_is_release(self):
        for sd_name, data in self.rel.blobs.items():
            self.assertEqual((self.folder / sd_name).read_bytes(), data, sd_name)
        self.assertEqual((self.folder / "manifest.json").read_bytes(), self.rel.manifest_bytes)
        self.assertEqual(self.dev.cmd(f"gate {DIR} 1")["gate"], "verified")
        self.assertEqual(self.dev.cmd(f"preflight {DIR}")["rc"], 0)

    def assert_gated_clean(self):
        st = self.dev.cmd("status")
        self.assertEqual(st["violations"], 0, "every SD op inside the io gate")
        self.assertEqual(st["refs"], 0, "no io ref leaked")
        return st

    def write_parse(self, obj) -> dict:
        p = self.work / f"m-{self._testMethodName}.json"
        p.write_text(obj if isinstance(obj, str) else json.dumps(obj))
        return self.dev.cmd(f"parse {p}")

    # ── manifest parsing ──
    def test_published_v140_manifest_parses(self):
        r = self.write_parse(REAL_V140)
        self.assertEqual(r["rc"], 0, r)
        self.assertEqual(r["version"], "1.4.0")
        self.assertEqual(r["sizes"], [13248, 3072, 8192, 449712])
        self.assertEqual(r["src"], ["bootloader.bin", "partitions.bin", "boot_app0.bin", "ambit-fw-v1.4.0.bin"])

    def test_manifest_bounds_and_shape_are_enforced(self):
        def variant(f):
            m = json.loads(json.dumps(REAL_V140))
            f(m)
            return m
        cases = {
            # A table reaching 0x9000 would overwrite NVS (per-unit calibration).
            "region_size_out_of_bounds": variant(lambda m: m["flash"][1].update(size=0x1001)),
            "duplicate_offset": variant(lambda m: m["flash"][2].update(offset="0x8000")),
            "unknown_offset": variant(lambda m: m["flash"][2].update(offset="0x9000")),
            "flash_not_four_regions": variant(lambda m: m["flash"].pop()),
            "not_esp32c3": variant(lambda m: m.update(chip="esp32")),
            "bad_version": variant(lambda m: m.update(version="1.4")),
            "bad_sha256": variant(lambda m: m["flash"][0].update(sha256="ab" * 31)),
            "bad_file_name": variant(lambda m: m["flash"][3].update(file="../app.bin")),
        }
        for why, m in cases.items():
            with self.subTest(why=why):
                r = self.write_parse(m)
                self.assertNotEqual(r["rc"], 0, r)
                self.assertEqual(r["why"], why)
        for name in ("a/b.bin", "app.bin?x=1", ".hidden", "x" * 64):
            with self.subTest(name=name):
                r = self.write_parse(variant(lambda m: m["flash"][3].update(file=name)))
                self.assertEqual(r["why"], "bad_file_name", r)
        self.assertEqual(self.write_parse("not json")["why"], "not_json_object")

    # ── fetch ──
    def test_fetch_writes_the_verified_release_then_reuses_it(self):
        self.rel.serve(self.dev)
        r = self.fetch()
        self.assertEqual((r["rc"], r["why"], r["downloaded"], r["reused"]), (0, "ok", 4, 0), r)
        self.assertEqual(r["bytes"], sum(s for *_x, s in LAYOUT))
        self.assertEqual(sorted(self.files()), sorted([*self.rel.blobs, "manifest.json"]))
        self.assert_folder_is_release()
        # Second run: only the manifest is downloaded again.
        r = self.fetch()
        self.assertEqual((r["rc"], r["downloaded"], r["reused"], r["opens"]), (0, 0, 4, 1), r)
        self.assert_gated_clean()

    def test_hand_staged_legacy_folder_is_adopted_without_downloading(self):
        self.folder.mkdir(parents=True)
        for n, d in self.rel.blobs.items():
            (self.folder / n).write_bytes(d)
        self.assertEqual(self.dev.cmd(f"gate {DIR} 1")["gate"], "legacy")
        self.assertEqual(self.dev.cmd(f"preflight {DIR}")["rc"], 0, "legacy folders keep the old rule")
        self.rel.serve(self.dev)
        r = self.fetch()
        self.assertEqual((r["rc"], r["downloaded"], r["reused"]), (0, 0, 4), r)
        self.assert_folder_is_release()

    def test_wrong_region_on_card_is_replaced(self):
        self.folder.mkdir(parents=True)
        (self.folder / "app.bin").write_bytes(b"old image" * 100)
        self.rel.serve(self.dev)
        r = self.fetch()
        self.assertEqual((r["rc"], r["removed"], r["downloaded"]), (0, 1, 4), r)
        self.assert_folder_is_release()

    def test_new_build_of_same_version_replaces_manifest_and_changed_region(self):
        self.rel.serve(self.dev)
        self.assertEqual(self.fetch()["rc"], 0)
        self.rel = Release(self.work / "rel-rebuilt", seed=2)   # every blob differs
        # keep three regions identical to the first build, change only the app
        first = Release(self.work / f"rel-{self._testMethodName}")
        for sd_name, asset, _o, _s in LAYOUT[:3]:
            (self.rel.dir / asset).write_bytes(first.blobs[sd_name])
            self.rel.blobs[sd_name] = first.blobs[sd_name]
            entry = next(e for e in self.rel.manifest["flash"] if e["file"] == asset)
            entry["sha256"] = sha(first.blobs[sd_name])
        self.rel.manifest_bytes = json.dumps(self.rel.manifest, indent=2).encode()
        (self.rel.dir / "manifest.json").write_bytes(self.rel.manifest_bytes)
        self.rel.serve(self.dev)
        r = self.fetch()
        self.assertEqual((r["rc"], r["removed"], r["downloaded"], r["reused"]), (0, 1, 1, 3), r)
        self.assert_folder_is_release()

    def test_requested_version_must_match_manifest(self):
        self.rel.serve(self.dev)
        r = self.fetch(ver="1.4.1")
        self.assertEqual((r["rc"], r["why"]), (ESP_FAIL, "manifest_version_mismatch"), r)
        self.assertFalse((self.state / "sdcard" / "ambit_fw" / "1.4.1").exists())
        self.assertEqual(self.files(), [])

    def test_server_bytes_that_disagree_with_the_manifest_are_never_kept(self):
        bad = self.work / "bad-app.bin"
        bad.write_bytes(bytes(b ^ 0x5A for b in self.rel.blobs["app.bin"]))   # same size, other bytes
        self.rel.serve(self.dev, **{"ambit-fw-v1.4.0.bin": bad})
        r = self.fetch()
        self.assertEqual((r["rc"], r["why"]), (ESP_FAIL, "region_sha256_mismatch"), r)
        self.assertNotIn("app.bin", self.files())
        self.assertEqual(self.dev.cmd(f"gate {DIR} 1")["why"], "region_missing")
        self.assertEqual(self.dev.cmd(f"preflight {DIR}")["rc"], ESP_ERR_NOT_FOUND)
        self.assert_gated_clean()

    def test_network_cut_mid_region_leaves_nothing_flashable_and_resumes(self):
        self.rel.serve(self.dev, **{"ambit-fw-v1.4.0.bin": (20000, -2, 200)})
        r = self.fetch()
        self.assertEqual((r["rc"], r["why"], r["downloaded"]), (ESP_FAIL, "region_read_failed", 3), r)
        self.assertNotIn("app.bin", self.files())
        self.assertNotEqual(self.dev.cmd(f"preflight {DIR}")["rc"], 0)
        self.rel.serve(self.dev)
        r = self.fetch()
        self.assertEqual((r["rc"], r["downloaded"], r["reused"]), (0, 1, 3), r)
        self.assert_folder_is_release()

    def test_http_failures_touch_nothing(self):
        for asset, spec, why in (("manifest.json", None, "manifest_http_failed"),
                                 ("boot_app0.bin", (-1, -2, 404), "region_http_failed"),
                                 ("partitions.bin", (-1, 9999, 200), "region_size_mismatch")):
            with self.subTest(asset=asset):
                self.dev.finish()
                self.dev = L.new_dev(self.exe, self.state)
                self.dev.start()
                self.rel.serve(self.dev, **({asset: spec} if spec else {}))
                url = MANIFEST_URL if spec else BASE + "missing/manifest.json"
                r = self.fetch(url=url)
                self.assertEqual((r["rc"], r["why"]), (ESP_FAIL, why), r)
                self.assertNotEqual(self.dev.cmd(f"preflight {DIR}")["rc"], 0)
        self.assert_gated_clean()

    def test_oversized_manifest_is_refused(self):
        big = self.work / "big.json"
        m = dict(self.rel.manifest, padding="x" * 5000)
        big.write_text(json.dumps(m))
        self.dev.cmd(f"serve {MANIFEST_URL} {big}")
        r = self.fetch()
        self.assertEqual(r["why"], "manifest_too_large", r)

    # ── SD faults and resets ──
    def test_sd_write_faults_never_report_success_and_leave_no_bad_region(self):
        for op, mode, why, diag in (("fwrite", "eio", "region_write_failed", "write"),
                                    ("fwrite", "short", "region_write_failed", "write"),
                                    ("fsync", "eio", "region_sync_failed", "fsync"),
                                    ("fclose", "eio", "region_close_failed", "close"),
                                    ("fread", "flip_read", "region_readback_failed", "verify")):
            with self.subTest(op=op, mode=mode):
                self.dev.finish()
                self.dev = L.new_dev(self.exe, self.state)
                self.dev.start()
                self.rel.serve(self.dev)
                arm = self.dev.cmd(f"arm {op} {mode} 1 1 app.bin")
                self.assertEqual(arm["arm"], "ok")
                r = self.fetch()
                self.assertEqual((r["rc"], r["why"]), (ESP_FAIL, why), r)
                self.assertNotIn("app.bin", self.files())
                self.assertNotEqual(self.dev.cmd(f"preflight {DIR}")["rc"], 0)
                st = self.assert_gated_clean()
                self.assertGreaterEqual(st["diag"]["faults"].get(f"ambit_ota.{diag}", 0), 1, st["diag"])
                # the next attempt on a healthy card completes
                r = self.fetch()
                self.assertEqual(r["rc"], 0, r)
                self.assert_folder_is_release()

    def test_reset_mid_region_write_is_refused_by_gate_then_repaired(self):
        self.rel.serve(self.dev)
        self.assertEqual(self.dev.cmd("arm fwrite crash_mid 20 1 app.bin")["arm"], "ok")
        r = self.fetch()
        self.assertIn("crashed", r, "the simulated CPU reset must interrupt the fetch")
        # After the reset: manifest committed first, app.bin is a short file.
        self.assertIn("manifest.json", self.files())
        self.assertIn("app.bin", self.files())
        self.assertLess((self.folder / "app.bin").stat().st_size, self.rel.manifest["flash"][3]["size"])
        self.dev.start()
        self.assertEqual(self.dev.cmd(f"gate {DIR} 0")["gate"], "rejected", "boot sync's size scan refuses it")
        self.assertEqual(self.dev.cmd(f"preflight {DIR}")["rc"], ESP_ERR_INVALID_CRC,
                         "all four files present, yet the flash refuses them")
        self.rel.serve(self.dev)
        r = self.fetch()
        self.assertEqual((r["rc"], r["removed"], r["downloaded"], r["reused"]), (0, 1, 1, 3), r)
        self.assert_folder_is_release()
        self.assert_gated_clean()

    def test_reset_right_after_each_create_never_yields_a_flashable_folder(self):
        # Per region: open #1 is step 2's check (ENOENT), open #2 the exclusive
        # create: the reset lands with that file empty and later ones missing.
        for name in ("manifest.json", *self.rel.blobs):
            with self.subTest(name=name):
                self.dev.finish()
                self.dev = L.new_dev(self.exe, self.state)
                self.dev.start()
                self.rel.serve(self.dev)
                self.dev.cmd(f"arm fopen crash_after 2 1 {name}")
                r = self.fetch()
                self.assertIn("crashed", r)
                self.dev.start()
                pre = self.dev.cmd(f"preflight {DIR}")["rc"]
                self.assertNotEqual(pre, 0, "an interrupted fetch never leaves a folder the flash accepts")
                self.rel.serve(self.dev)
                self.assertEqual(self.fetch()["rc"], 0)
                self.assert_folder_is_release()

    def test_folder_name_must_match_manifest_version(self):
        self.rel.serve(self.dev)
        self.assertEqual(self.fetch()["rc"], 0)
        other = self.state / "sdcard" / "ambit_fw" / "1.4.1"
        other.mkdir()
        for n in os.listdir(self.folder):
            (other / n).write_bytes((self.folder / n).read_bytes())
        g = self.dev.cmd("gate ./sdcard/ambit_fw/1.4.1 1")
        self.assertEqual((g["gate"], g["why"]), ("rejected", "manifest_version_mismatch"))


class SourceContractTest(unittest.TestCase):
    """Orderings the host build cannot see (they live in ambit_ota.c / ambit_flash.c)."""

    def test_region_table_matches_the_flash_writer(self):
        flash = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        table = flash[flash.index("static const flash_region_t s_regions[]"):]
        table = table[:table.index("};")]
        writer = re.findall(r'\{\s*(0x[0-9a-fA-F]+),\s*"([a-z_0-9.]+)"\s*\}', table)
        man = (ROOT / "components/ambit_flash/ambit_fw_manifest.c").read_text()
        layout = man[man.index("s_layout[AMBIT_FW_NUM_REGIONS] = {"):]
        layout = re.findall(r'\{\s*(0x[0-9a-fA-F]+),\s*"([a-z_0-9.]+)"', layout[:layout.index("};")])
        self.assertEqual([(int(o, 16), n) for o, n in writer], [(int(o, 16), n) for o, n in layout])

    def test_fetch_runs_in_the_mqtt_blackout_and_before_any_ambit_is_touched(self):
        src = (ROOT / "components/ambit_ota/ambit_ota.c").read_text()
        fn = src.split("static void ambit_do_flash(", 1)[1].split("static void ambit_ota_run(", 1)[0]
        fetch = fn.index("ambit_fw_fetch(")
        self.assertLess(fn.index("s_cfg.comms_suspend()"), fetch)
        self.assertLess(fetch, fn.index("ambit_flash_probe("))
        self.assertLess(fetch, fn.index("ambit_flash_image("))
        self.assertIn("!sd_ok || !fetch_ok", fn)

    def test_plain_http_manifest_urls_are_refused(self):
        src = (ROOT / "components/ambit_ota/ambit_ota.c").read_text()
        fn = src.split("esp_err_t ambit_ota_request_flash(", 1)[1]
        self.assertIn('strncmp(manifest_url, "https://", 8) != 0', fn)

    def test_boot_sync_target_scan_uses_the_gate(self):
        src = (ROOT / "components/ambit_flash/ambit_flash.c").read_text()
        fn = src.split("static bool dir_has_all_regions(", 1)[1].split("esp_err_t ambit_flash_find_target(", 1)[0]
        self.assertIn("ambit_fw_dir_check(dir, false, NULL)", fn)
        self.assertIn("AMBIT_FW_DIR_LEGACY || g == AMBIT_FW_DIR_VERIFIED", fn)


if __name__ == "__main__":
    unittest.main()

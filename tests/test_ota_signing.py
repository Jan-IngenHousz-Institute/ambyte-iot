import configparser
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization
from flash_gui.firmware_metadata import ImageError, decode_manifest, inspect_image, key_id, padded_intermediate, sign_image
from tools.check_release_signing import NO_EFUSE, REQUIRED, bench_id, check_config, check_release, load_pin, private_key, reject_private_material
from tools.prepare_signed_release import prepare
from tools.upload_guard import validate_upload
from tools.publish_release_units import publish_units
from ota_fixture import image


class SigningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.other = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.pin = key_id(cls.key)
        cls.raw = image()
        cls.signed = sign_image(cls.raw, cls.key)
        cls.bench = private_key((ROOT / "tools/keys/UNSAFE_BENCH_ONLY_firmware.pem").read_bytes())
        cls.config = {**dict.fromkeys(REQUIRED, True), **dict.fromkeys(NO_EFUSE, False),
                      "SECURE_SIGNED_ON_BOOT": False, "SECURE_BOOT_BUILD_SIGNED_BINARIES": False}

    def test_real_signature_and_sizes(self):
        info = inspect_image(self.signed, require_signed=True, require_manifest=True)
        self.assertEqual(info.raw_size, len(self.raw))
        self.assertEqual(info.padded_size, 4096)
        self.assertEqual(len(self.signed), 8192)
        self.assertEqual(info.key_id, self.pin)
        self.assertEqual(info.storage.readable, (1,))
        vectors=json.loads((ROOT/"docs/signed-image-vectors.json").read_text())
        for vector in vectors["decode"]:
            decoded=decode_manifest(bytes.fromhex(vector["hex"]))
            self.assertEqual(f"{sum(1<<v for v in decoded.readable)},{decoded.written},{decoded.fallback}",vector["result"])
        check_release(self.signed, self.pin)

    def test_unsigned_with_option_is_not_a_release(self):
        check_config(self.config)
        with self.assertRaisesRegex(ImageError, "unsigned"):
            check_release(padded_intermediate(self.raw), self.pin)

    def test_public_bench_root_rejected_even_if_requested_pin_matches(self):
        data = sign_image(self.raw, self.bench)
        for pin in (self.pin, bench_id()):
            with self.subTest(pin=pin), self.assertRaises(ImageError):
                check_release(data, pin)

    def test_wrong_key_with_genuine_signature(self):
        other = sign_image(self.raw, self.other)
        self.assertEqual(inspect_image(other).key_id, key_id(self.other))
        with self.assertRaisesRegex(ImageError, "reviewed release pin"):
            check_release(other, self.pin)

    def test_code_and_manifest_tampering_fail_before_manifest_read(self):
        for offset in (0x120 + 8, 0x120 + 32 + 16):
            changed = bytearray(self.signed)
            changed[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaisesRegex(ImageError, "signature image digest"):
                check_release(bytes(changed), self.pin)

    def test_exactly_one_signature_block_and_crc(self):
        for offset in (4096 + 1216, 4096 + 1196, 4096 + 1200):
            changed = bytearray(self.signed)
            changed[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ImageError):
                check_release(bytes(changed), self.pin)
        for n in (4097, 5000, 8191):
            with self.subTest(length=n), self.assertRaises(ImageError):
                check_release(self.signed[:n], self.pin)

    def test_no_efuse_and_no_build_key_settings(self):
        check_config(self.config)
        for flag in NO_EFUSE + ("SECURE_SIGNED_ON_BOOT", "SECURE_BOOT_BUILD_SIGNED_BINARIES"):
            with self.subTest(flag=flag), self.assertRaises(ImageError):
                check_config({**self.config, flag: True})
        for flag in REQUIRED:
            with self.subTest(flag=flag), self.assertRaises(ImageError):
                check_config({**self.config, flag: False})
        with self.assertRaises(ImageError):
            check_config({**self.config, "SECURE_BOOT_SIGNING_KEY": "UNSAFE_BENCH_ONLY_firmware.pem"})

    def test_placeholder_missing_and_bench_pins_fail(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "pin"
            with self.assertRaises(ImageError): load_pin(p)
            for value in ("UNCONFIGURED", "", "fw-rsa3072:" + "0" * 64, bench_id()):
                p.write_text(value)
                with self.subTest(pin=value), self.assertRaises(ImageError): load_pin(p)
            p.write_text(self.pin + "\n")
            self.assertEqual(load_pin(p), self.pin)

    def test_private_material_not_generic_tls_pem_marker(self):
        reject_private_material(b"-----BEGIN PRIVATE KEY-----\0mbedTLS parser marker")
        with self.assertRaises(ImageError):
            reject_private_material((ROOT / "tools/keys/UNSAFE_BENCH_ONLY_firmware.pem").read_bytes())

    def test_fleet_rollout_reads_and_authenticates_complete_artifact(self):
        from tools.fleet_deploy import fleet_deploy
        for data,accepted in ((self.signed,True),(self.raw,False),(sign_image(self.raw,self.bench),False),
                              (sign_image(self.raw,self.other),False)):
            with self.subTest(accepted=accepted),patch("tools.check_release_signing.load_pin",return_value=self.pin),\
                 patch.object(fleet_deploy.urllib.request,"urlopen",return_value=io.BytesIO(data)):
                if accepted:fleet_deploy.verify_firmware_url("https://example.test/release.bin")
                else:
                    with self.assertRaises(ImageError):fleet_deploy.verify_firmware_url("https://example.test/release.bin")

    def _bundle(self, root, app=None):
        app = self.raw if app is None else app
        (root / "firmware.bin").write_bytes(self.raw)
        with zipfile.ZipFile(root / "ambyte-iot-v2.8.0-test.zip", "w") as z:
            z.writestr("ambyte-iot.bin", app)
            z.writestr("bootloader/bootloader.bin", b"old unsigned bootloader")
            z.writestr("partition_table/partition-table.bin", b"fixed partitions")
            z.writestr("ota_data_initial.bin", b"old otadata")
            z.writestr("flasher_args.json", b'{"flash_files":{}}')
        return root / "ambyte-iot-v2.8.0-test.zip"

    def _pem(self, key=None):
        return (key or self.key).private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()).decode()

    def test_promote_replaces_only_application_and_verifies_published_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); bundle = self._bundle(root)
            with zipfile.ZipFile(bundle) as z: before = {n: z.read(n) for n in z.namelist()}
            pin = root / "pin"; pin.write_text(self.pin)
            prepare(root, "2.8.0-test", self._pem(), pin)
            published = (root / "firmware.bin").read_bytes()
            check_release(published, self.pin)
            with zipfile.ZipFile(bundle) as z:
                self.assertEqual(z.read("ambyte-iot.bin"), published)
                for name, old in before.items():
                    if name != "ambyte-iot.bin": self.assertEqual(z.read(name), old)

    def test_promotion_failure_never_mutates_intermediate(self):
        for case in ("absent", "wrong_key", "wrong_version", "mismatch", "resign", "placeholder"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as td:
                root = Path(td); bundle = self._bundle(root, b"wrong" if case == "mismatch" else None)
                if case == "resign": self._bundle(root, self.signed); (root / "firmware.bin").write_bytes(self.signed)
                pin = root / "pin"; pin.write_text("UNCONFIGURED" if case == "placeholder" else self.pin)
                before = (root / "firmware.bin").read_bytes(), bundle.read_bytes()
                with self.assertRaises(ImageError):
                    prepare(root, "wrong" if case == "wrong_version" else "2.8.0-test",
                            "" if case == "absent" else self._pem(self.other if case == "wrong_key" else self.key), pin)
                self.assertEqual(((root / "firmware.bin").read_bytes(), bundle.read_bytes()), before)

    def test_every_environment_has_one_guard_and_unsigned_upload_is_refused(self):
        config = configparser.ConfigParser()
        config.read(ROOT / "platformio.ini")
        environments = {s[4:] for s in config.sections() if s.startswith("env:")}
        self.assertEqual(environments, {"esp32-s3-devkitm-1", "bench", "svc-bench", "evq-hil",
            "sensors-recovery-hil", "bench-spiram-internal", "bench-wifi-ps-none", "bench-pm-none"})
        def scripts(section):
            if config.has_option(section,"extra_scripts"):return config[section]["extra_scripts"]
            return scripts(config[section]["extends"])
        for environment in environments:
            with self.subTest(environment=environment), tempfile.TemporaryDirectory() as td:
                self.assertIn("pre:tools/extra_script.py",scripts("env:"+environment))
                root = Path(td); (root / "config").mkdir()
                (root / "config/sdkconfig.json").write_text(json.dumps(self.config))
                (root / "firmware.bin").write_bytes(self.raw)
                with self.assertRaisesRegex(ImageError, "unsigned"): validate_upload(root)
                (root / "firmware.bin").write_bytes(self.signed)
                validate_upload(root)

    def test_scons_tools_namespace_collision_still_registers_upload_guard(self):
        code='''
import os,sys,types
from pathlib import Path
sys.modules['tools']=types.ModuleType('tools')
class Environment:
    def __getitem__(self,key):
        assert key=='PROJECT_DIR'
        return str(Path.cwd())
    def subst(self,key):
        assert key=='$BUILD_DIR'
        return '/tmp/ambyte-scons-guard-test'
    def AddPreAction(self,target,action):
        assert target=='upload' and callable(action)
        print('UPLOAD_GUARD_REGISTERED')
namespace={'env':Environment(),'Import':lambda name:None}
source=Path('tools/extra_script.py')
exec(compile(source.read_text(),str(source),'exec'),namespace)
'''
        p=subprocess.run([sys.executable,"-c",code],cwd=ROOT,capture_output=True,text=True,
                         env={**os.environ,"AMBYTE_NVS_SKIP":"1"})
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(p.stdout.count("UPLOAD_GUARD_REGISTERED"),1)

    def test_mixed_release_failure_calls_neither_publisher_and_schedule_only_needs_no_key(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            self._bundle(root)
            with patch("tools.publish_release_units.subprocess.run") as runner:
                # Real prepare entrypoint is called; the actual repository pin
                # is intentionally unconfigured until Dominik supplies the root.
                with self.assertRaises(ImageError):
                    publish_units(root,"2.8.0-test",True,"",runner=runner)
                runner.assert_not_called()
                publish_units(root,"",True,"",runner=runner)
                self.assertEqual(runner.call_count,1)
                self.assertEqual(runner.call_args.kwargs["cwd"],ROOT/"schedule")


if __name__ == "__main__": unittest.main()

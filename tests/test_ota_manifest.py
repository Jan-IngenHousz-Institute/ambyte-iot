import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ManifestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "manifest"
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-g", "-Icomponents/ota_policy/include",
            "components/ota_policy/ota_manifest.c", "tests/ota_host/manifest_driver.c", "-o", str(cls.binary)],
            cwd=ROOT, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_case(self, *args):
        p = subprocess.run([str(self.binary), *map(str,args)], capture_output=True, text=True)
        self.assertEqual(p.returncode,0,p.stderr)
        return p.stdout.strip()

    def test_documented_byte_and_compatibility_vectors(self):
        vectors = json.loads((ROOT / "docs/signed-image-vectors.json").read_text())
        for v in vectors["decode"]:
            with self.subTest(v=v): self.assertEqual(self.run_case("decode",v["hex"]),v["result"])
        for v in vectors["decision"]:
            with self.subTest(v=v): self.assertEqual(self.run_case("decide",*v["input"]),v["result"])

    def test_every_reserved_or_header_byte_and_truncation(self):
        good=bytes.fromhex("414d425954454d31010020000900000002000000010000000100000000000000")
        self.assertEqual(self.run_case("decode",good.hex()),"2,1,1")
        for index in list(range(16))+list(range(28,32)):
            b=bytearray(good);b[index]^=1
            with self.subTest(index=index):self.assertEqual(self.run_case("decode",b.hex()),"invalid")
        for size in range(32):
            with self.subTest(size=size):self.assertEqual(self.run_case("decode",good[:size].hex()),"invalid")


class RefusalTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)/"ota";cls.binary=Path(cls.temp.name)/"refusal"
        includes=[ROOT/"tests/ota_host/include",ROOT/"tests/svc_host/jobs_include",ROOT/"tests/svc_host/include",
                  ROOT/"tests/identity_host/include",ROOT/"components/ota_policy/include"]
        p=subprocess.run([os.environ.get("CC","clang"),"-std=c11","-D_POSIX_C_SOURCE=200809L",
            f'-DOTA_REFUSAL_DIRECTORY="{cls.root}"',"-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-g",
            *(f"-I{i}" for i in includes),str(ROOT/"components/ota_policy/ota_refusal.c"),str(ROOT/"tests/ota_host/refusal_driver.c"),
            *(f"-Wl,--wrap={f}" for f in ("fopen","fwrite","fflush","fsync","fclose","rename")),"-o",str(cls.binary)],capture_output=True,text=True)
        if p.returncode:raise RuntimeError(p.stderr)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def command(self,*args):
        p=subprocess.run([str(self.binary),*args],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr);return p.stdout.strip()

    def test_faults_preserve_previous_record_and_restart_readback(self):
        for fault in ("open","write","flush","fsync","close","rename"):
            with self.subTest(fault=fault):
                self.assertEqual(self.command("signature_invalid"),"saved")
                before=(self.root/"refusal.bin").read_bytes()
                self.assertEqual(self.command("fallback_cannot_read_written",fault),"failed")
                self.assertEqual((self.root/"refusal.bin").read_bytes(),before)
                policy=json.loads(self.command("read").removeprefix("policy: "))
                self.assertEqual(policy["last_refusal"]["reason"],"signature_invalid")
                self.assertEqual(policy["last_refusal"]["id"],'id"with\\escapes')
                self.assertLessEqual(sum(f.stat().st_size for f in self.root.iterdir()),512)

    def test_missing_corrupt_and_short_record_are_unavailable(self):
        self.root.mkdir(exist_ok=True)
        path=self.root/"refusal.bin"
        if path.exists():path.unlink()
        self.assertEqual(json.loads(self.command("read").removeprefix("policy: "))["refusal_state"],"missing")
        self.command("signature_invalid");data=bytearray(path.read_bytes());data[100]^=1;path.write_bytes(data)
        policy=json.loads(self.command("read").removeprefix("policy: "))
        self.assertEqual(policy["refusal_state"],"corrupt");self.assertIsNone(policy["last_refusal"])
        path.write_bytes(b"short")
        self.assertEqual(json.loads(self.command("read").removeprefix("policy: "))["refusal_state"],"unavailable")

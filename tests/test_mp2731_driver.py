"""Exercise real MP2731 adapter failure boundaries under ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class Mp2731DriverTest(unittest.TestCase):
    def test_contention_partial_reads_and_preview(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'driver'
            subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-DESP_ERR_TIMEOUT=0x107',
                f'-I{ROOT}/tests/mp2731_driver_stubs',f'-I{ROOT}/tests/host_stubs',
                f'-I{ROOT}/components/domain/include',f'-I{ROOT}/components/mp2731/include',
                str(ROOT/'tests/mp2731_driver_host.c'),str(ROOT/'components/mp2731/mp2731_core.c'),
                str(ROOT/'components/mp2731/mp2731_diagnostics.c'),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('DRIVER_HISTORY_OK',result.stdout)

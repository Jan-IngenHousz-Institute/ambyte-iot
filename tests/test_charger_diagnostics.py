"""Qualify read-only scans, interrupted reporting and sampled charger history."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ChargerDiagnosticsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        binary = Path(cls.tmp.name) / 'charger'
        subprocess.run([os.environ.get('CC', shutil.which('clang') or 'cc'),
            '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            '-fno-sanitize-recover=all',
            f'-I{ROOT}/tests/host_stubs', f'-I{ROOT}/components/domain/include',
            f'-I{ROOT}/components/mp2731/include', str(ROOT / 'tests/charger_diagnostics_host.c'),
            str(ROOT / 'components/mp2731/mp2731_diagnostics.c'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise AssertionError(f"duplicate JSON key: {key}")
                result[key] = value
            return result
        cls.records = dict((k, json.loads(v, object_pairs_hook=unique_object)) for k,v in
                           (line.split('=',1) for line in result.stdout.splitlines()))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_readback_and_decodes(self):
        d = self.records['FULL']
        self.assertEqual(d['read_mask'], 0xFFFFFF)
        self.assertEqual(d['registers']['17'], 128)
        self.assertEqual(d['iin_limit_ma'], 500)
        self.assertEqual(d['vin_min_mv'], 4300)
        self.assertEqual(d['ntc_pct'], 39.984)
        self.assertEqual(d['ntc_fault'], 6)
        self.assertTrue(d['thermal_regulation'])
        self.assertTrue(d['safety_timer_expired'])
        self.assertEqual(d['adc_freshness'], 'unverified')

    def test_partial_read_never_invents_zero_ntc_or_timer(self):
        d = self.records['PARTIAL']
        self.assertNotEqual(d['read_error'], 0)
        self.assertNotIn('10', d['registers'])
        self.assertNotIn('ntc_pct', d)
        self.assertNotIn('safety_timer_expired', d)
        self.assertEqual(self.records['BUS_ERROR']['registers'], {})
        self.assertIn('history', self.records['BUS_ERROR'])

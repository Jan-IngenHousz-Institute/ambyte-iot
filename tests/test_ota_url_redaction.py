"""Presigned update URLs carry credentials; application logs must not print them."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("components/ota_update/ota_update.c", "components/ambit_ota/ambit_ota.c",
           "components/command_router/command_router.c",
           "components/script_update/script_update.c")
STRING = r'"(?:\\.|[^"\\])*"'


class OtaUrlRedactionTest(unittest.TestCase):
    def test_ota_and_ambit_logs_never_format_or_pass_a_raw_url(self):
        for source in SOURCES:
            calls = re.findall(r'\bESP_LOG[A-Z]\((?:' + STRING + r'|[^";])*;',
                               (ROOT / source).read_text(), re.S)
            self.assertTrue(calls, source)
            for call in calls:
                # The router also handles unrelated replay commands. Schedule
                # URL installs need the same credential protection as OTA.
                if source.endswith("command_router.c") and not re.search(r'"(?:ota_update|ambit_ota|script_update)\b', call):
                    continue
                with self.subTest(source=source, call=call):
                    formats = "".join(re.findall(STRING, call))
                    self.assertNotRegex(formats, r'(?:url|dir)=%s')
                    arguments = re.sub(STRING, '""', call)
                    # Only the numeric length may reach the log formatter.
                    arguments = re.sub(r'\bstrlen\(\s*(?:(?:r->)?(?:url|dir)|r->text)\s*\)',
                                       'URL_LENGTH', arguments)
                    self.assertNotRegex(arguments, r'\b(?:url|dir)\b|\br->text\b')

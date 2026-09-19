"""Host-side checks for the SD archive inventory scanner behind the
`evlog_inventory` MQTT command.

Compiles the REAL components/event_log/evlog_inventory.c against
tests/host_stubs, builds a synthetic /archive + /events tree in a temp dir
(tests/evlog_inventory_host.c) and asserts the counts, bounds, window filter,
torn/unparsed handling, io-gate balance and the rendered JSON reply.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class EvlogInventoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="evlog-inventory-host-")
        binary = Path(cls.tmp.name) / "evlog_inventory_host"
        compile_cmd = [
            os.environ.get("CC", shutil.which("clang") or "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{ROOT / 'tests/host_stubs'}",
            f"-I{ROOT / 'components/event_log/include'}",
            str(ROOT / "tests/evlog_inventory_host.c"),
            str(ROOT / "components/event_log/evlog_inventory.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(compile_cmd, check=True, cwd=ROOT)
        data_dir = Path(cls.tmp.name) / "card"
        data_dir.mkdir()
        env = os.environ.copy()
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        result = subprocess.run(
            [str(binary), str(data_dir)], check=True, cwd=ROOT, capture_output=True, text=True, env=env,
        )
        cls.kv = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition("=")
            cls.kv[key] = value

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_counts_and_bounds(self) -> None:
        kv = self.kv
        self.assertEqual(kv["present"], "1")
        self.assertEqual(kv["files"], "3")                # arc-100, arc-200, arc-300-7
        self.assertEqual(kv["other_entries"], "1")        # notes.txt; dotfile ignored
        self.assertEqual(kv["records"], "10")             # 5 + 3 + 2 complete lines
        self.assertEqual(kv["torn"], "1")
        self.assertEqual(kv["unparsed"], "1")             # 202: start_ms beyond the head
        self.assertEqual(kv["pre_2024"], "1")             # 102
        self.assertEqual(kv["min_id"], "100")
        self.assertEqual(kv["max_id"], "301")             # 202 unparsed, 203 torn
        self.assertEqual(kv["min_start_ms"], "86400000")
        self.assertEqual(kv["max_start_ms"], "1758200060000")
        self.assertEqual(kv["legacy_files"], "2")

    def test_window_filter(self) -> None:
        # from_id 101 .. to_id 201 → 101,102,103,104 from the first file, 200,201 from the second
        self.assertEqual(self.kv["in_window"], "6")
        self.assertEqual(self.kv["listed"], "3")
        self.assertEqual(self.kv["list0"], "100:100:104:5:4")
        self.assertEqual(self.kv["list1"], "200:200:201:3:2")

    def test_io_gate_balanced_and_per_file(self) -> None:
        # one listing + three files + one legacy listing
        self.assertEqual(self.kv["io_calls"], "5")

    def test_rendered_json(self) -> None:
        doc = json.loads(self.kv["JSON"])
        self.assertEqual(doc["type"], "evlog_inventory")
        self.assertEqual(doc["id"], 'inv-1"};alert(1)\\')      # escaped, not truncated
        self.assertEqual(doc["device_id"], "28:37:2F:FF:FC:80")
        self.assertTrue(doc["ok"])
        self.assertEqual(doc["archive"]["records"], 10)
        self.assertEqual(doc["archive"]["in_window"], 6)
        self.assertEqual(doc["archive"]["pre_2024"], 1)
        self.assertEqual(doc["store"]["pending"], 135)
        self.assertEqual(doc["store"]["last_acked_id"], 26466)
        self.assertEqual(doc["window"], {"from_id": 101, "to_id": 201, "from_ms": 0, "to_ms": 0})
        self.assertEqual(len(doc["files_list"]), 3)
        self.assertEqual(doc["files_list"][1]["last_id"], 201)
        self.assertEqual(doc["files_list"][2]["name_id"], 300)
        self.assertEqual(self.kv["ABSENT_OK"], "1")


if __name__ == "__main__":
    unittest.main()

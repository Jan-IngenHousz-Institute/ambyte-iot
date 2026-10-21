# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the production sensor fault scenarios with seeded transition interleavings."""
import argparse
from pathlib import Path
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_sensors import build, SCENARIOS

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix", action="store_true", required=True)
    parser.add_argument("--seeds", default="1,7")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="sensor-matrix-") as tmp:
        binary = Path(tmp) / "sensor"
        build(binary)
        for seed in map(int, args.seeds.split(",")):
            cases = list(SCENARIOS)
            random.Random(seed).shuffle(cases)
            for case in cases:
                result = subprocess.run([binary, case, str(seed)], capture_output=True, timeout=30)
                if result.returncode:
                    sys.stderr.buffer.write(result.stderr)
                    raise SystemExit(f"FAIL seed={seed} case={case}")
                print(f"PASS seed={seed} case={case}")

# SPDX-License-Identifier: GPL-3.0-or-later
"""Make every new GUI/client harness part of the firmware's default discovery."""
import importlib.util
from pathlib import Path
import unittest


def load_tests(loader, tests, pattern):
    del tests, pattern
    path = Path(__file__).resolve().parents[1] / "flash_gui/tests/test_sensors.py"
    spec = importlib.util.spec_from_file_location("_sensor_gui_contract", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return loader.loadTestsFromModule(module)


if __name__ == "__main__":
    unittest.main()

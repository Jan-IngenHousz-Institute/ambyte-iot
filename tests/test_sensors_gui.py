# SPDX-License-Identifier: GPL-3.0-or-later
"""Make every new GUI/client harness part of the firmware's default discovery."""
import importlib.util
from pathlib import Path
import unittest


# Both unittest and pytest collect this same TestCase. pytest does not implement
# unittest's load_tests hook, so hiding the class behind that hook loses coverage.
path = Path(__file__).resolve().parents[1] / "flash_gui/tests/test_sensors.py"
spec = importlib.util.spec_from_file_location("_sensor_gui_contract", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SensorClientTest = module.SensorClientTest


if __name__ == "__main__":
    unittest.main()

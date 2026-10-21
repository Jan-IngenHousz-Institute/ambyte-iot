"""Locate the qualified SDK by metadata, independent of PIO directory spelling."""
import json
import os
from pathlib import Path
import unittest

VERSION = "3.50500.0"
SOURCES = ("components/json/cJSON/cJSON.c",
           "components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c",
           "components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c",
           "components/console/linenoise/linenoise.c",
           "components/console/linenoise/linenoise.h")


def pinned_idf():
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    packages = Path(os.environ.get("PLATFORMIO_PACKAGES_DIR", core / "packages"))
    for candidate in sorted(packages.glob("framework-espidf*")):
        try:
            metadata = json.loads((candidate / "package.json").read_text())
        except (OSError, ValueError):
            continue
        if (metadata.get("name") == "framework-espidf" and metadata.get("version") == VERSION and
                all((candidate / source).is_file() for source in SOURCES)):
            return candidate
    message = (f"Pinned SDK platformio/framework-espidf@{VERSION} with qualified sources missing under {packages}; "
               "run pio pkg install -e esp32-s3-devkitm-1")
    if os.environ.get("AMBYTE_REQUIRE_PINNED_IDF") == "1":
        raise RuntimeError(message + " (AMBYTE_REQUIRE_PINNED_IDF=1: SDK coverage is required)")
    raise unittest.SkipTest(message)

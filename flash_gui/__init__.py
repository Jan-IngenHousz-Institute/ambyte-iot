# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""ambyte flash GUI: cross-platform flash + provision tool for ambyte boards.

One operator onboards boards one after another: sign in to openJII once (API
key), pick environment + experiment once, then per board: pick the port, click
"On-board device", confirm the name. The tool downloads the latest firmware
release, bakes a per-board NVS image (identity, certs, RTC seed, timezone,
MQTT), flashes everything with esptool, and verifies the result over the
USB-Serial-JTAG console.

Run with:  python -m flash_gui
"""


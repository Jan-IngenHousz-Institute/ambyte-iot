# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""PyInstaller entry point.

`python -m flash_gui` stays the dev entry (__main__.py, relative imports);
PyInstaller needs a plain script with ABSOLUTE imports because a script passed
to it has no package context — run it as:

    pyinstaller --paths . ... flash_gui/launcher.py
"""

import multiprocessing
import sys

from flash_gui.packaged_smoke import run_littlefs_smoke

if __name__ == "__main__":
    # Frozen-app guard: without it, any future multiprocessing use would
    # re-launch the GUI instead of a worker. Costs nothing today.
    multiprocessing.freeze_support()
    if "--smoke-littlefs" in sys.argv:
        run_littlefs_smoke()
    else:
        # Keep the GUI import out of the packaged smoke path: the smoke must
        # isolate littlefs rather than fail first on an unrelated Tk install.
        from flash_gui.gui import main

        main()

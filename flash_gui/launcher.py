"""PyInstaller entry point.

`python -m flash_gui` stays the dev entry (__main__.py, relative imports);
PyInstaller needs a plain script with ABSOLUTE imports because a script passed
to it has no package context — run it as:

    pyinstaller --paths . ... flash_gui/launcher.py
"""

import multiprocessing

from flash_gui.gui import main

if __name__ == "__main__":
    # Frozen-app guard: without it, any future multiprocessing use would
    # re-launch the GUI instead of a worker. Costs nothing today.
    multiprocessing.freeze_support()
    main()

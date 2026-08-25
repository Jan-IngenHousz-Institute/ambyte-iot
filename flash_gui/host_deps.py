# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Host-side dependency preflight.

Field case (2026-08-25, three times in one day, three different interpreters):
the GUI was launched from a Python that lacked `littlefs-python`, and the miss
surfaced only at step 3 — AFTER openJII had already rotated the board's
certificate — as the cryptic "FAILED at nvs: littlefs image: No module named
'littlefs'". Running from source means "whatever `python` resolves to today",
and a PC restart, a new venv, or a PlatformIO shell silently changes that.

So: every runtime import is checked up front, before the window opens and
again before any board-mutating step, and the report names the interpreter
that is missing them plus the exact command that fixes it. The packaged .exe
bundles everything and passes trivially.
"""

from __future__ import annotations

import importlib
import sys

# (import name, pip distribution name) — the pip name is what the operator
# must type, and for littlefs it differs from the module name.
RUNTIME_DEPS: tuple[tuple[str, str], ...] = (
    ("esptool", "esptool"),
    ("serial", "pyserial"),
    ("tzlocal", "tzlocal"),
    ("certifi", "certifi"),
    ("littlefs", "littlefs-python"),
)


class HostDependencyError(RuntimeError):
    """One or more runtime packages are not importable by this interpreter."""


def missing_host_dependencies() -> list[str]:
    """pip names of the runtime packages this interpreter cannot import."""
    missing = []
    for module, pip_name in RUNTIME_DEPS:
        try:
            importlib.import_module(module)
        except Exception:      # ImportError, but also a broken native ext.
            missing.append(pip_name)
    return missing


def describe_missing(missing: list[str]) -> str:
    return (
        f"This Python ({sys.executable}) is missing: {', '.join(missing)}.\n\n"
        "Install them for THIS interpreter and restart the GUI:\n\n"
        f'  "{sys.executable}" -m pip install -r flash_gui/requirements.txt\n\n'
        "Nothing was changed on any board."
    )


def require_host_dependencies() -> None:
    """Raise HostDependencyError with the operator-facing text if anything is
    missing. Called at GUI start and before each board's first mutating step,
    so a dependency problem can never surface mid-procedure."""
    missing = missing_host_dependencies()
    if missing:
        raise HostDependencyError(describe_missing(missing))

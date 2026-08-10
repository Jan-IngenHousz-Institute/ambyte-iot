"""TLS trust configuration shared by every HTTPS client in the GUI.

Frozen Python applications cannot rely on the build machine's OpenSSL CA
paths existing on the operator's Mac or Linux PC.  certifi is bundled by
PyInstaller and provides a portable Mozilla root store.  Load it in addition
to the platform defaults so locally installed roots (for example a company
proxy CA) continue to work as well.
"""

from __future__ import annotations

import ssl
from functools import lru_cache

import certifi


@lru_cache(maxsize=1)
def ssl_context() -> ssl.SSLContext:
    """Return the process-wide HTTPS context with portable CA roots loaded."""
    context = ssl.create_default_context()
    context.load_verify_locations(cafile=certifi.where())
    return context

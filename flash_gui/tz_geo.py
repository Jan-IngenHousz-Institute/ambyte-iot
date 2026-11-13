# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Offline coordinate -> IANA timezone lookup, identical to the firmware's.

The Ambyte resolves its own zone from its stored deployment coordinates
(components/timezone/tz_geo.c, docs/timezone-from-location.md). This module reads
the very same generated quadtree (flash_gui/tz_geo_data.py is byte-identical to
components/timezone/tz_geo.bin), so the GUI can predict exactly which zone a
device will pick for a coordinate -- e.g. to flag a provisioning timezone that
disagrees with the deployment site before the unit ships.

    zone_for(-25.3444, 131.0369)  -> "Australia/Darwin"
    zone_for(0.0, -30.0)          -> None   (open ocean)

Accuracy: within roughly 5 km of a zone border a point may resolve to the
neighbouring zone; points shortly beyond territorial waters may resolve to the
coastal zone. See tools/gen_tz_geo.py for the representation and the bound.
Pure Python, no third-party dependency; the data is decoded once, lazily.
"""

from __future__ import annotations

import base64
import functools
import hashlib
import math
import struct

__all__ = ["zone_for", "is_unset_location"]

_NONE = 0xFFFF
_HEADER = struct.Struct("<4sBBBBHH8s32sIII")


def is_unset_location(lat: float, lon: float) -> bool:
    """(0, 0) is the "never set" placeholder some tools emit; the firmware
    never resolves a zone from it (it is open ocean anyway)."""
    return lat == 0.0 and lon == 0.0


@functools.lru_cache(maxsize=1)
def _blob() -> bytes:
    from flash_gui import tz_geo_data
    blob = base64.b64decode("".join(tz_geo_data.BLOB_B64))
    if hashlib.sha256(blob).hexdigest() != tz_geo_data.BLOB_SHA256:
        raise RuntimeError("flash_gui/tz_geo_data.py is corrupt (SHA-256 mismatch)")
    return blob


def _cell_index(value: float, origin: float, span: float, depth: int) -> int:
    # Same IEEE-754 double expression as tz_geo.c, so both pick the same cell.
    n = 1 << depth
    index = math.floor((value + origin) / span * n)
    return min(max(index, 0), n - 1)


def _lookup(blob: bytes, lat: float, lon: float) -> str | None:
    """Resolve one point against a format-1 blob; None for ocean / bad data."""
    if len(blob) < _HEADER.size:
        return None
    (magic, version, tile_depth, max_depth, _coast, n_names, _reserved, _release,
     _sha, names_off, tiles_off, size) = _HEADER.unpack_from(blob)
    if magic != b"TZG1" or version != 1 or size != len(blob) or not 0 < tile_depth <= max_depth <= 24:
        return None
    x = _cell_index(lon, 180.0, 360.0, max_depth)
    y = _cell_index(lat, 90.0, 180.0, max_depth)
    shift = max_depth - tile_depth
    tile = (y >> shift) * (1 << tile_depth) + (x >> shift)
    n_tiles = 1 << (2 * tile_depth)
    if tiles_off + 4 * n_tiles > size:
        return None
    start = struct.unpack_from("<I", blob, tiles_off + 4 * tile)[0]
    end = size if tile + 1 == n_tiles else struct.unpack_from("<I", blob, tiles_off + 4 * tile + 4)[0]
    if not tiles_off + 4 * n_tiles <= start < end <= size:
        return None
    k = blob[start]
    if k == 0 or start + 1 + 2 * k > end:
        return None
    palette = struct.unpack_from(f"<{k}H", blob, start + 1)
    width = k.bit_length()
    bitpos = (start + 1 + 2 * k) * 8
    limit = end * 8

    def read() -> int | None:
        nonlocal bitpos
        if bitpos + width > limit:
            return None
        value = 0
        for _ in range(width):
            value = (value << 1) | ((blob[bitpos >> 3] >> (7 - (bitpos & 7))) & 1)
            bitpos += 1
        return value

    depth = tile_depth
    while True:
        symbol = read()
        if symbol is None or symbol > k:
            return None
        if symbol:
            zone = palette[symbol - 1]
            break
        if depth >= max_depth:
            return None  # an internal node below the finest level is corrupt
        level = max_depth - depth - 1
        child = ((y >> level) & 1) * 2 + ((x >> level) & 1)
        for _ in range(child):  # skip the preceding siblings' subtrees
            pending = 1
            while pending:
                s = read()
                if s is None or s > k:
                    return None
                pending += 3 if s == 0 else -1
        depth += 1
    if zone == _NONE or zone >= n_names:
        return None
    name_at = names_off + struct.unpack_from("<H", blob, names_off + 2 * zone)[0]
    name_end = blob.find(b"\0", name_at, tiles_off)
    if name_end < 0:
        return None
    return blob[name_at:name_end].decode("ascii")


def zone_for(lat: float, lon: float) -> str | None:
    """IANA zone the Ambyte firmware resolves for a coordinate, or None.

    None means: invalid input (non-number, bool, non-finite, |lat| > 90,
    |lon| > 180), the unset (0, 0) placeholder, or open ocean. Every returned
    name is a member of flash_gui.tz_zone_table.ZONES.
    """
    if isinstance(lat, bool) or isinstance(lon, bool):
        return None
    try:
        lat = float(lat)
        lon = float(lon)
    except (TypeError, ValueError):
        return None
    if not (math.isfinite(lat) and math.isfinite(lon)
            and -90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        return None
    if is_unset_location(lat, lon):
        return None
    return _lookup(_blob(), lat, lon)

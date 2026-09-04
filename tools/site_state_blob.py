# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Canonical wire format for the atomic ``device_cfg/site_state`` NVS blob.

The firmware, command writers, and both provisioning-image builders must agree
byte-for-byte. The fixed layout deliberately avoids native struct padding:

    0..3   magic ``AMST``
    4      format version (1)
    5      presence flags (lat, lon, deployment)
    6      deployment UTF-8 byte length (0..63)
    7      reserved, zero
    8..15  latitude, IEEE-754 binary64 little-endian (zero when absent)
    16..23 longitude, IEEE-754 binary64 little-endian (zero when absent)
    24..87 deployment UTF-8 bytes + NUL + zero padding

One NVS key is the power-loss unit: a reset can select the old blob or the new
blob, never three independently committed site fields.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass


SITE_STATE_MAGIC = b"AMST"
SITE_STATE_VERSION = 1
SITE_STATE_SIZE = 88
SITE_STATE_DEPLOYMENT_CAP = 63
SITE_STATE_FLAG_LAT = 1 << 0
SITE_STATE_FLAG_LON = 1 << 1
SITE_STATE_FLAG_DEPLOYMENT = 1 << 2
SITE_STATE_KNOWN_FLAGS = (
    SITE_STATE_FLAG_LAT | SITE_STATE_FLAG_LON | SITE_STATE_FLAG_DEPLOYMENT
)


@dataclass(frozen=True)
class SiteState:
    lat: float | None
    lon: float | None
    deployment: str | None


def encode_site_state(
    lat: float | None = None,
    lon: float | None = None,
    deployment: str | None = None,
) -> bytes:
    """Encode optional site fields into the firmware's fixed version-1 blob."""
    if lat is not None and (not math.isfinite(lat) or not -90.0 <= lat <= 90.0):
        raise ValueError("lat must be between -90 and 90")
    if lon is not None and (not math.isfinite(lon) or not -180.0 <= lon <= 180.0):
        raise ValueError("lon must be between -180 and 180")

    deployment_bytes = b""
    if deployment is not None:
        if "\0" in deployment:
            raise ValueError("deployment cannot contain NUL")
        deployment_bytes = deployment.encode("utf-8")
        if len(deployment_bytes) > SITE_STATE_DEPLOYMENT_CAP:
            raise ValueError("deployment must fit the firmware's 63-byte buffer")

    blob = bytearray(SITE_STATE_SIZE)
    blob[0:4] = SITE_STATE_MAGIC
    blob[4] = SITE_STATE_VERSION
    flags = 0
    if lat is not None:
        flags |= SITE_STATE_FLAG_LAT
        struct.pack_into("<d", blob, 8, lat)
    if lon is not None:
        flags |= SITE_STATE_FLAG_LON
        struct.pack_into("<d", blob, 16, lon)
    if deployment is not None:
        flags |= SITE_STATE_FLAG_DEPLOYMENT
        blob[6] = len(deployment_bytes)
        blob[24:24 + len(deployment_bytes)] = deployment_bytes
        # byte 24 + len remains the required NUL because the blob is zero-filled
    blob[5] = flags
    return bytes(blob)


def decode_site_state(blob: bytes) -> SiteState:
    """Validate and decode a blob; reject non-canonical/corrupt versions."""
    if len(blob) != SITE_STATE_SIZE:
        raise ValueError(f"site_state must be exactly {SITE_STATE_SIZE} bytes")
    if blob[0:4] != SITE_STATE_MAGIC:
        raise ValueError("site_state magic is invalid")
    if blob[4] != SITE_STATE_VERSION:
        raise ValueError(f"unsupported site_state version {blob[4]}")
    flags = blob[5]
    if flags & ~SITE_STATE_KNOWN_FLAGS or blob[7] != 0:
        raise ValueError("site_state flags/reserved byte are invalid")

    lat = struct.unpack_from("<d", blob, 8)[0]
    lon = struct.unpack_from("<d", blob, 16)[0]
    if flags & SITE_STATE_FLAG_LAT:
        if not math.isfinite(lat) or not -90.0 <= lat <= 90.0:
            raise ValueError("site_state latitude is invalid")
    elif blob[8:16] != bytes(8):
        raise ValueError("absent site_state latitude must be zero")
    if flags & SITE_STATE_FLAG_LON:
        if not math.isfinite(lon) or not -180.0 <= lon <= 180.0:
            raise ValueError("site_state longitude is invalid")
    elif blob[16:24] != bytes(8):
        raise ValueError("absent site_state longitude must be zero")

    deployment_len = blob[6]
    deployment_field = blob[24:88]
    if flags & SITE_STATE_FLAG_DEPLOYMENT:
        if deployment_len > SITE_STATE_DEPLOYMENT_CAP:
            raise ValueError("site_state deployment length is invalid")
        if b"\0" in deployment_field[:deployment_len]:
            raise ValueError("site_state deployment contains an early NUL")
        if deployment_field[deployment_len:] != bytes(64 - deployment_len):
            raise ValueError("site_state deployment is not NUL-terminated/padded")
        try:
            deployment = deployment_field[:deployment_len].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError("site_state deployment is not UTF-8") from exc
    else:
        if deployment_len != 0 or deployment_field != bytes(64):
            raise ValueError("absent site_state deployment must be zero")
        deployment = None

    return SiteState(
        lat=lat if flags & SITE_STATE_FLAG_LAT else None,
        lon=lon if flags & SITE_STATE_FLAG_LON else None,
        deployment=deployment,
    )

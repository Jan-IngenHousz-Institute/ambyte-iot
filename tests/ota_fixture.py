"""Minimal real ESP32-S3 image for signing/metadata/flash guard tests."""
import hashlib
import struct


def image(version="2.8.0-test", modern=True):
    from flash_gui.firmware_metadata import MANIFEST_V1
    header = bytearray(24)
    header[0:4] = bytes([0xE9, 2, 0, 0x40])
    struct.pack_into("<H", header, 12, 9)
    header[23] = 1
    payload = bytearray(256)
    payload[0:4] = bytes.fromhex("3254cdab")
    payload[16:48] = version.encode().ljust(32, b"\0")
    payload.extend(MANIFEST_V1 if modern else bytes(32))
    payload.extend(bytes(range(128)))
    content = header + struct.pack("<II", 0x3C000020, len(payload)) + payload
    code=bytes(range(128))
    content.extend(struct.pack("<II",0x42000020,len(code))+code)
    checksum = 0xEF
    for b in payload:
        checksum ^= b
    for b in code:
        checksum ^= b
    while (len(content) + 1) % 16:
        content.append(0)
    content.append(checksum)
    content.extend(hashlib.sha256(content).digest())
    return bytes(content)

"""ESP32-S3 signed application format shared by flashing and release tooling.

SBv2 layout follows ESP-IDF 5.5/espsecure: RSA3072 key fields and signature
integers are little endian; PSS uses SHA256/MGF1 and a 32-byte salt. A valid
embedded key proves consistency only. Release trust requires a reviewed pin.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import struct
import zlib

MANIFEST_OFFSET = 0x120
MANIFEST = struct.Struct("<8sHHHHIIII")
MANIFEST_V1 = MANIFEST.pack(b"AMBYTEM1", 1, 32, 9, 0, 2, 1, 1, 0)
SECTOR = 4096
BLOCK = 1216
MAX_SIGNED_BYTES = 2700 * 1024


class ImageError(ValueError):
    pass


@dataclass(frozen=True)
class StorageManifest:
    readable: tuple[int, ...]
    written: int
    fallback: int


@dataclass(frozen=True)
class ImageMetadata:
    version: str
    raw_size: int
    padded_size: int
    signature_present: bool
    key_id: str | None
    storage: StorageManifest | None


def decode_manifest(data: bytes) -> StorageManifest:
    if len(data) != 32:
        raise ImageError("candidate_manifest_invalid")
    magic, schema, length, target, reserved, readable, written, fallback, flags = MANIFEST.unpack(data)
    if (magic != b"AMBYTEM1" or schema != 1 or length != 32 or target != 9 or reserved or flags
            or not 1 <= written <= 31 or not 1 <= fallback <= 31
            or readable & 1 or not readable & (1 << written)):
        raise ImageError("candidate_manifest_invalid")
    return StorageManifest(tuple(i for i in range(1, 32) if readable & (1 << i)), written, fallback)


def app_layout(data: bytes) -> tuple[int, int]:
    """Return checksum offset and hashed image end, including its ordinary SHA."""
    if len(data) < 32 or data[0] != 0xE9 or not 1 <= data[1] <= 16:
        raise ImageError("invalid ESP image header")
    if struct.unpack_from("<H", data, 12)[0] != 9:
        raise ImageError("image target is not ESP32-S3")
    off = 24
    for _ in range(data[1]):
        if off + 8 > len(data):
            raise ImageError("truncated segment header")
        length = struct.unpack_from("<I", data, off + 4)[0]
        off += 8 + length
        if off > len(data):
            raise ImageError("truncated segment")
    checksum = (off + 16) // 16 * 16 - 1
    end = checksum + 1 + (32 if data[23] else 0)
    if data[23] != 1 or end > len(data):
        raise ImageError("missing ordinary image SHA256")
    return checksum, end


def _check_image(data: bytes) -> tuple[int, int]:
    checksum, end = app_layout(data)
    value, off = 0xEF, 24
    for _ in range(data[1]):
        n = struct.unpack_from("<I", data, off + 4)[0]
        for b in data[off + 8:off + 8 + n]:
            value ^= b
        off += n + 8
    if data[checksum] != value or hashlib.sha256(data[:end - 32]).digest() != data[end - 32:end]:
        raise ImageError("image checksum/hash mismatch")
    first_len = struct.unpack_from("<I", data, 28)[0]
    if first_len < 256 or data[32:36] != bytes.fromhex("3254cdab"):
        raise ImageError("missing app descriptor")
    return checksum, end


def key_material(key) -> bytes:
    from cryptography.hazmat.primitives.asymmetric import rsa
    public = key.public_key() if hasattr(key, "private_numbers") else key
    if not isinstance(public, rsa.RSAPublicKey) or public.key_size != 3072:
        raise ImageError("firmware signing requires an RSA-3072 key")
    numbers = public.public_numbers()
    if numbers.e != 65537:
        raise ImageError("RSA exponent must be 65537")
    n = numbers.n
    return (n.to_bytes(384, "little") + struct.pack("<I", numbers.e)
            + pow(2, 6144, n).to_bytes(384, "little")
            + struct.pack("<I", (-pow(n, -1, 1 << 32)) & 0xFFFFFFFF))


def key_id(key) -> str:
    return "fw-rsa3072:" + hashlib.sha256(key_material(key)).hexdigest()


def verify_signature(data: bytes, padded: int) -> str:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils
    if len(data) != padded + SECTOR:
        raise ImageError("missing/truncated/noncanonical signature sector")
    sector = data[padded:]
    block = sector[:BLOCK]
    if block[:4] != b"\xe7\x02\0\0" or block[1200:1216] != bytes(16):
        raise ImageError("invalid RSA SBv2 block0 header")
    if zlib.crc32(block[:1196]) != struct.unpack_from("<I", block, 1196)[0]:
        raise ImageError("invalid signature block CRC")
    if sector[BLOCK:] != b"\xff" * (SECTOR - BLOCK):
        raise ImageError("extra signature blocks or noncanonical sector padding")
    digest = hashlib.sha256(data[:padded]).digest()
    if block[4:36] != digest:
        raise ImageError("signature image digest mismatch")
    n = int.from_bytes(block[36:420], "little")
    e = struct.unpack_from("<I", block, 420)[0]
    try:
        public = rsa.RSAPublicNumbers(e, n).public_key()
        if key_material(public) != block[36:812]:
            raise ImageError("invalid RSA accelerator key material")
        public.verify(block[812:1196][::-1], digest,
                      padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                      utils.Prehashed(hashes.SHA256()))
    except (ValueError, InvalidSignature) as exc:
        raise ImageError("RSA-PSS signature verification failed") from exc
    return key_id(public)


def inspect_image(data: bytes, *, require_signed: bool = False,
                  require_manifest: bool = False) -> ImageMetadata:
    _, raw = app_layout(data)
    padded = (raw + SECTOR - 1) // SECTOR * SECTOR
    # Verify a present signature BEFORE reading attacker-controlled manifest.
    signed = len(data) > padded
    signer = verify_signature(data, padded) if signed else None
    _check_image(data)
    if require_signed and not signed:
        raise ImageError("unsigned image: sign the secure-padded intermediate before flashing")
    field = data[MANIFEST_OFFSET:MANIFEST_OFFSET + 32]
    modern = field.startswith(b"AMBYTE")
    storage = decode_manifest(field) if modern or require_manifest else None
    if modern and not signed and require_signed:
        raise ImageError("modern image has no valid signature")
    version_bytes = data[48:80].split(b"\0", 1)[0]
    try:
        version = version_bytes.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ImageError("invalid application version") from exc
    return ImageMetadata(version, raw, padded, signed, signer, storage)


def padded_intermediate(data: bytes) -> bytes:
    info = inspect_image(data, require_manifest=True)
    if info.signature_present:
        raise ImageError("refusing to sign an already signed image")
    if len(data) not in (info.raw_size, info.padded_size):
        raise ImageError("unexpected intermediate trailer")
    if data[info.raw_size:] != b"\xff" * (len(data) - info.raw_size):
        raise ImageError("noncanonical secure padding")
    return data + b"\xff" * (info.padded_size - len(data))


def sign_image(data: bytes, key, *, fixture_manifest: bool = False) -> bytes:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, utils
    if fixture_manifest:
        # Bench-only invalid-manifest fixtures still need a genuine signature
        # so policy, rather than an unrelated hash failure, refuses them.
        _, raw = _check_image(data)
        if len(data) != raw:
            raise ImageError("fixture signer requires an ordinary unsigned image")
        body = data + b"\xff" * ((-len(data)) % SECTOR)
    else:
        body = padded_intermediate(data)
    digest = hashlib.sha256(body).digest()
    signature = key.sign(digest, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                         utils.Prehashed(hashes.SHA256()))
    block = b"\xe7\x02\0\0" + digest + key_material(key) + signature[::-1]
    block += struct.pack("<I", zlib.crc32(block)) + bytes(16)
    result = body + block + b"\xff" * (SECTOR - len(block))
    verify_signature(result, len(body))
    if not fixture_manifest:
        inspect_image(result, require_signed=True, require_manifest=True)
    return result


def rewrite_fixture(data: bytes, *, version: str, manifest: bytes | None = None) -> bytes:
    """Bench fixtures only: rebuild ordinary checksum/SHA before test signing."""
    _, raw = _check_image(data)
    output = bytearray(data[:raw])
    encoded = version.encode("ascii")
    if not 0 < len(encoded) < 32:
        raise ImageError("fixture version exceeds app descriptor")
    output[48:80] = encoded.ljust(32, b"\0")
    if manifest is not None:
        if len(manifest) != 32:
            raise ImageError("fixture manifest length")
        output[MANIFEST_OFFSET:MANIFEST_OFFSET + 32] = manifest
    return repair_fixture_checksums(bytes(output))


def repair_fixture_checksums(data: bytes) -> bytes:
    """Bench mutations only: repair public checksums, preserving any signature.

    A signature-negative fixture must reach the signature check on IDF, whose
    ordinary image checksum check runs first. This never repairs or re-signs
    the signature sector, padding or its digest.
    """
    output = bytearray(data)
    checksum, end = app_layout(output)
    value, off = 0xEF, 24
    for _ in range(output[1]):
        n = struct.unpack_from("<I", output, off + 4)[0]
        for b in output[off + 8:off + 8 + n]:
            value ^= b
        off += n + 8
    output[checksum] = value
    output[end - 32:end] = hashlib.sha256(output[:end - 32]).digest()
    return bytes(output)


NO_EFUSE = ("SECURE_DISABLE_ROM_DL_MODE", "SECURE_ENABLE_SECURE_ROM_DL_MODE",
            "SECURE_FLASH_ENC_ENABLED", "SECURE_BOOT", "BOOTLOADER_APP_ANTI_ROLLBACK",
            "BOOT_ROM_LOG_ALWAYS_OFF", "BOOT_ROM_LOG_ON_GPIO_LOW", "BOOT_ROM_LOG_ON_GPIO_HIGH")
REQUIRED = ("SECURE_SIGNED_APPS_NO_SECURE_BOOT", "SECURE_SIGNED_APPS_RSA_SCHEME",
            "SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT", "BOOTLOADER_APP_ROLLBACK_ENABLE")


def check_config(config: dict) -> None:
    if not isinstance(config, dict):
        raise ImageError("effective sdkconfig is not an object")
    for key in NO_EFUSE + ("SECURE_SIGNED_ON_BOOT", "SECURE_BOOT_BUILD_SIGNED_BINARIES"):
        if config.get(key, False) is not False:
            raise ImageError(f"unsafe effective config: {key}")
    for key in REQUIRED:
        if config.get(key) is not True:
            raise ImageError(f"required effective config missing: {key}")
    for key, value in config.items():
        if ("SIGNING_KEY" in key or "VERIFICATION_KEY" in key) and value:
            raise ImageError("signing keys must be supplied outside build configuration")

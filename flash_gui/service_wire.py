# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Strict bounded service codec, descriptors and safe outcome errors."""
from __future__ import annotations

import hashlib
import json
import re
import zlib

LINE_MAX, JSON_MAX, FRAME_MAX, CHUNK = 248, 1024, 1043, 144
MAX_ID = (1 << 64) - 1
ACTION_LIST = ("svc.version", "svc.bind", "svc.result", "svc.stats", "schedule.begin", "p",
               "schedule.commit", "svc.abort", "svc.wifi.join", "svc.wifi.status", "svc.exit")
STATES = {"staging", "connecting", "committing", "persisting", "succeeded", "failed", "aborted", "uncertain"}
TERMINAL = {"succeeded", "failed", "aborted", "uncertain"}
REASONS = {"", "bad_frame", "bad_crc", "line_too_long", "frame_timeout", "session_timeout",
           "invalid_args", "unsupported", "not_found", "expired", "request_conflict", "busy",
           "id_exhausted", "no_space", "storage_error", "corrupt_store", "chunk_crc", "offset_conflict",
           "offset_gap", "size_limit", "incomplete", "checksum_mismatch", "compile_failed",
           "activation_unverified", "aborted", "commit_started", "interrupted", "wifi_auth_rejected",
           "wifi_timeout", "wifi_failed", "wifi_not_retrying", "wifi_driver_unresolved",
           "wifi_invalid_credentials", "wifi_unavailable", "mode_required", "rx_overrun",
           "store_changed", "operation_complete", "invalid_sequence", "workbook_mismatch",
           "already_active", "unprovable"}
HEX16 = re.compile(r"[0-9a-f]{16}\Z")
SHA = re.compile(r"[0-9a-f]{64}\Z")
TOKEN = re.compile(r"[A-Za-z0-9_.+-]{1,31}\Z")
UUID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\Z")


class ServiceClientError(RuntimeError):
    """Safe to show to an operator: messages never contain transport bytes."""


class TransportError(ServiceClientError):
    pass


class ProtocolError(TransportError):
    pass


class PlainConsole(ProtocolError):
    pass


class ServiceError(ServiceClientError):
    def __init__(self, reason: str):
        self.reason = reason
        super().__init__(f"Device service: {reason}.")


class UnknownOutcome(ServiceClientError):
    pass


class TransferCancelled(ServiceClientError):
    pass


class BusyOperation(ServiceClientError):
    def __init__(self, kind: str, state: str):
        self.kind, self.state = kind, state
        super().__init__(f"Device busy with another client's {kind} operation ({state}); nothing was changed.")


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _constant(value):
    raise ValueError("nonfinite JSON value")


def _json(data: bytes):
    return json.loads(data.decode("utf-8"), object_pairs_hook=_object, parse_constant=_constant)


def _hex(value, allow_zero=True):
    return isinstance(value, str) and bool(HEX16.fullmatch(value)) and (allow_zero or int(value, 16) != 0)


def _integer(value, low, high):
    return type(value) is int and low <= value <= high


def _truth(value):
    return value is None or type(value) is bool


def _reason(value):
    # Never reflect arbitrary device text into an operator error. In particular
    # an old/buggy peer echoing a credential cannot turn it into a log message.
    return isinstance(value, str) and value in REASONS


def _ssid(value):
    try:
        return isinstance(value, str) and 1 <= len(value.encode("utf-8")) <= 31 and "\0" not in value
    except UnicodeError:
        return False


def _keys(result, required, optional=()):
    if not isinstance(result, dict) or not set(required) <= result.keys() or result.keys() - set(required) - set(optional):
        raise ProtocolError("Device service response has an unexpected schema.")


def schedule_descriptor(blob: bytes, sha256: str, version: str, built_fw: str,
                        workbook: str | None = None) -> dict:
    if (not isinstance(blob, bytes) or not 1 <= len(blob) <= 16384 or
            not isinstance(sha256, str) or not SHA.fullmatch(sha256) or
            hashlib.sha256(blob).hexdigest() != sha256 or
            not isinstance(version, str) or not TOKEN.fullmatch(version) or
            not isinstance(built_fw, str) or not TOKEN.fullmatch(built_fw) or
            (workbook is not None and (not isinstance(workbook, str) or not UUID.fullmatch(workbook)))):
        raise ServiceClientError("Schedule bytes, digest or release descriptor are invalid.")
    return dict(kind="schedule", size=len(blob), sha256=sha256, workbook=workbook,
                version=version, built_fw=built_fw)


def encode_frame(correlation: int, body: str) -> bytes:
    if not _integer(correlation, 1, 0xffffffff):
        raise ServiceClientError("Invalid service correlation.")
    try:
        raw = f"{correlation:08x} {body}".encode("ascii")
    except (UnicodeError, ValueError):
        raise ServiceClientError("Service request is not an ASCII token line.") from None
    if any(byte < 32 or byte > 126 for byte in raw):
        raise ServiceClientError("Service request is not an ASCII token line.")
    line = raw + b"*%08x\n" % zlib.crc32(raw)
    if len(line) > LINE_MAX:
        raise ServiceClientError("Service request exceeds the line bound.")
    return line


def decode_frame(line: bytes) -> tuple[int, dict]:
    if len(line) > FRAME_MAX or not line.endswith(b"\n") or b"\r" in line or b"\0" in line:
        raise ProtocolError("Invalid service frame boundary.")
    match = re.fullmatch(rb"([0-9a-f]{8}) (.{1,1024})\*([0-9a-f]{8})\n", line)
    if not match:
        # Used only during nonsecret mode discovery. Established sessions never
        # treat this as a usable response or extract JSON from inside the line.
        if any(token in line for token in (b"ambyte>", b"Unrecognized command", b"svc.version", b"svc\r\n")):
            raise PlainConsole("Device is answering through the plain console.")
        raise ProtocolError("Invalid service frame.")
    prefix = line[:-10]
    if zlib.crc32(prefix) != int(match[3], 16):
        raise ProtocolError("Service response checksum failed.")
    try:
        result = _json(match[2])
    except (ValueError, UnicodeError, RecursionError):
        raise ProtocolError("Service response is not strict JSON.") from None
    if (not isinstance(result, dict) or type(result.get("ok")) is not bool or
            (result.get("store") is not None and not _hex(result.get("store"), False)) or "store" not in result):
        raise ProtocolError("Invalid service response envelope.")
    return int(match[1], 16), result

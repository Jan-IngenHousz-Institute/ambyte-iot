# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Nonsecret recovery intent and process ownership for one gateway MAC."""
from __future__ import annotations

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import tempfile
import threading

from .service_wire import (ServiceClientError, SHA, TOKEN, UUID, _hex, _integer, _json, _ssid)


class OperationJournal:
    """One current intent and one preserved previous intent per gateway.

    Neither file contains schedule bytes or credentials. The MAC, rather than
    the OS port name, prevents a newly attached board inheriting an old upload.
    """
    def __init__(self, directory: Path, mac: str):
        if not isinstance(mac, str) or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", mac):
            raise ServiceClientError("A stable gateway MAC is required for service recovery.")
        self.mac = mac.upper()
        self.path = Path(directory) / (self.mac.replace(":", "") + ".json")
        self._owner = None

    @contextmanager
    def lease(self):
        """OS-owned lock releases on process exit, including an app-close kill.

        A stale filename is harmless; there is no PID file to guess away. A
        second process cannot replace the only recovery intent for this MAC.
        """
        owner = threading.get_ident()
        if self._owner == owner:
            yield
            return
        if self._owner is not None:
            raise ServiceClientError("Another transfer owns this gateway's saved intent.")
        stream = None
        acquired = False
        try:
            self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            stream = self.path.with_suffix(".lock").open("a+b")
            if os.name == "nt":
                import msvcrt
                stream.seek(0, os.SEEK_END)
                if stream.tell() == 0:
                    stream.write(b"\0"); stream.flush()
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            acquired = True
            self._owner = owner
        except OSError:
            if stream is not None:
                stream.close()
            raise ServiceClientError("Cannot own the saved transfer intent; another GUI may be using this gateway.") from None
        try:
            yield
        finally:
            self._owner = None
            if acquired:
                try:
                    if os.name == "nt":
                        stream.seek(0)
                        msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
                    else:
                        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
                except OSError:
                    pass  # closing the descriptor also releases its OS lock
                finally:
                    stream.close()

    def load(self) -> dict | None:
        try:
            with self.path.open("rb") as source:
                data = source.read(4097)
            if len(data) > 4096:
                raise ValueError("journal bound")
            value = _json(data)
            if (not isinstance(value, dict) or set(value) != {"schema", "mac", "store", "op", "descriptor", "phase"} or
                    value["schema"] != 1 or type(value["schema"]) is not int or value["mac"] != self.mac or
                    not _hex(value["store"], False) or not _hex(value["op"], False) or
                    value["phase"] not in {"reserved", "admitted", "committing", "complete", "uncertain"}):
                raise ValueError("journal schema")
            d = value["descriptor"]
            if not isinstance(d, dict) or d.get("kind") not in {"schedule", "wifi"}:
                raise ValueError("descriptor")
            if d["kind"] == "schedule":
                if (set(d) != {"kind", "size", "sha256", "workbook", "version", "built_fw"} or
                        not _integer(d["size"], 1, 16384) or not isinstance(d["sha256"], str) or not SHA.fullmatch(d["sha256"]) or
                        not isinstance(d["version"], str) or not TOKEN.fullmatch(d["version"]) or
                        not isinstance(d["built_fw"], str) or not TOKEN.fullmatch(d["built_fw"]) or
                        (d["workbook"] is not None and (not isinstance(d["workbook"], str) or not UUID.fullmatch(d["workbook"])))):
                    raise ValueError("schedule descriptor")
            elif set(d) != {"kind", "ssid"} or not _ssid(d["ssid"]):
                raise ValueError("wifi descriptor")
            return value
        except FileNotFoundError:
            return None
        except (OSError, ValueError, UnicodeError, TypeError, RecursionError):
            raise ServiceClientError("Cannot read the saved transfer intent; preserve it before starting another operation.") from None

    def save(self, value: dict) -> None:
        temporary = None
        try:
            data = json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode()
            if len(data) > 4096:
                raise ValueError("journal bound")
            self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(prefix=self.path.name + ".", dir=self.path.parent)
            with os.fdopen(fd, "wb") as destination:
                destination.write(data)
                destination.flush()
                os.fsync(destination.fileno())
            os.replace(temporary, self.path)
            temporary = None
            if os.name == "posix":
                directory = os.open(self.path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
        except (OSError, ValueError, TypeError):
            raise ServiceClientError("Cannot persist the transfer intent; no new operation will be sent.") from None
        finally:
            if temporary is not None:
                try:
                    os.unlink(temporary)
                except OSError:
                    pass

    def preserve(self) -> None:
        try:
            if self.path.exists():
                os.replace(self.path, self.path.with_suffix(".previous.json"))
        except OSError:
            raise ServiceClientError("Cannot preserve the previous transfer intent.") from None

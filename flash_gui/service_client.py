# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded USB service client shared by the GUI and bench tools.

The serial handle belongs to the caller. This module never opens a port, never
changes DTR/RTS, never logs a request, and never downgrades a failed service
operation to a console mutation. A journal records intent before transmission;
after any ambiguous exchange the device's result is the only offset authority.
"""
from __future__ import annotations

import base64
import hashlib
from collections import deque
from functools import wraps
import re
import time
from typing import Callable
import zlib

from .service_journal import OperationJournal
from .service_wire import (
    ACTION_LIST, CHUNK, FRAME_MAX, JSON_MAX, LINE_MAX, MAX_ID, SHA, STATES, TERMINAL, TOKEN, UUID,
    BusyOperation, PlainConsole, ProtocolError, ServiceClientError, ServiceError, TransferCancelled,
    TransportError, UnknownOutcome, decode_frame, encode_frame, schedule_descriptor,
    _hex, _integer, _keys, _reason, _ssid, _truth,
)


def _journalled(function):
    @wraps(function)
    def run(self, *args, **kwargs):
        with self.journal.lease():
            return function(self, *args, **kwargs)
    return run


class ServiceClient:
    def __init__(self, transport, journal: OperationJournal, *, reconnect: Callable | None = None,
                 clock=time.monotonic, sleep=time.sleep):
        self.transport, self.journal, self.reconnect = transport, journal, reconnect
        self.clock, self.sleep = clock, sleep
        self.generation = 0
        self.correlation = 0
        self.store = None
        self.highest = 0
        self.active = False
        self.actions = frozenset()
        self._permission = None
        self._carry = bytearray()
        self._deadline = None
        self.wire_read = self.wire_written = 0
        self.round_trips = deque(maxlen=65536)
        self._settings_transport = None
        self._saved_settings = {}
        self._configure_transport()

    def _configure_transport(self):
        # pyserial bounds the blocking calls themselves; a monotonic loop alone
        # cannot bound a write with write_timeout=None on a disconnected port.
        if self._settings_transport is not self.transport:
            self._settings_transport = self.transport
            self._saved_settings = {name: getattr(self.transport, name) for name in ("timeout", "write_timeout") if hasattr(self.transport, name)}
        if hasattr(self.transport, "timeout"):
            self.transport.timeout = 0.1
        if hasattr(self.transport, "write_timeout"):
            self.transport.write_timeout = 5.0

    def invalidate(self):
        self.active = False
        self._permission = None
        self._carry.clear()

    def replace_transport(self, transport):
        self.invalidate()
        self.generation += 1
        self.transport = transport
        self._configure_transport()

    def _limit(self, seconds=5.0):
        return min(self.clock() + seconds, self._deadline if self._deadline is not None else float("inf"))

    def _write(self, data: bytes):
        try:
            if self._deadline is not None:
                remaining = self._deadline - self.clock()
                if remaining <= 0:
                    raise OSError("deadline")
                if hasattr(self.transport, "write_timeout"):
                    self.transport.write_timeout = min(5.0, remaining)
            if getattr(self.transport, "is_open", True) is False:
                raise OSError("closed")
            wrote = self.transport.write(data)
            if wrote != len(data):
                raise OSError("partial write")
            self.wire_written += wrote
        except Exception:
            self.invalidate()
            raise TransportError("USB write failed; recover the saved operation before retrying.") from None

    def _line(self, deadline):
        while self.clock() < deadline:
            end = self._carry.find(b"\n")
            if end >= 0:
                line = bytes(self._carry[:end + 1])
                del self._carry[:end + 1]
                return line
            if len(self._carry) >= FRAME_MAX:
                self.invalidate()
                raise ProtocolError("Service response exceeds the line bound.")
            try:
                waiting = getattr(self.transport, "in_waiting", 0)
                size = min(waiting or 1, FRAME_MAX - len(self._carry))
                if hasattr(self.transport, "timeout"):
                    self.transport.timeout = min(0.1, max(0, deadline - self.clock()))
                data = self.transport.read(size)
            except Exception:
                self.invalidate()
                raise TransportError("USB read failed; recover the saved operation before retrying.") from None
            if not isinstance(data, bytes) or len(data) > size:
                self.invalidate()
                raise ProtocolError("Invalid USB read boundary.")
            self.wire_read += len(data)
            self._carry.extend(data)
        self.invalidate()
        raise TransportError("Service response timed out; the device outcome may still be pending.")

    def _receive(self, correlation, *, allow_plain=False, probe=None):
        try:
            deadline, setup_bytes = self._limit(), 0
            while True:
                line = self._line(deadline)
                setup_bytes += len(line)
                if allow_plain and setup_bytes > 8192:
                    raise ProtocolError("Too much plain service discovery output.")
                if allow_plain and probe is not None and line in {probe, probe[:-1] + b"\r\n"}:
                    continue  # exact nonsecret request echo on older firmware
                if allow_plain and line in {b"Unrecognized command\n", b"Unrecognized command\r\n"}:
                    raise ServiceError("unsupported")
                if allow_plain and line in {b"\n", b"\r\n", b"ambyte> \r\n", b"ambyte> \n"}:
                    continue
                identifier, result = decode_frame(line)
                break
            if identifier == 0 and result.get("reason"):
                _keys(result, {"ok", "store", "reason"})
                if result["ok"] or not _reason(result["reason"]):
                    raise ProtocolError("Invalid service session notice.")
                if allow_plain and result["reason"] == "mode_required":
                    raise PlainConsole("Service mode entry is required.")
                raise ProtocolError("Service session ended or rejected a frame; reconnect and read the saved result.")
            if identifier != correlation:
                raise ProtocolError("Service response correlation does not match.")
            if self.store is not None and result["store"] != self.store:
                raise UnknownOutcome("Device service store changed; verify device state before an explicit new request.")
            if not result["ok"]:
                _keys(result, {"ok", "store", "reason"})
                if not _reason(result["reason"]) or not result["reason"]:
                    raise ProtocolError("Invalid service error reason.")
                if result["reason"] == "not_bound":
                    self.invalidate()
                    raise ProtocolError("Service session is not bound; rebind and reconcile the saved operation.")
                if result["reason"] == "store_changed":
                    raise UnknownOutcome("Device service store changed; verify the saved operation before a new request.")
                raise ServiceError(result["reason"])
            return result
        except (TransportError, UnknownOutcome):
            self.invalidate()
            raise

    def _exchange(self, body, *, discovery=False):
        """A framed exchange; action adapters validate their own result schema."""
        if body.split(" ", 1)[0] == "svc.wifi.join":
            if (not self.active or self._permission is None or
                    self._permission[:2] != (self.generation, self.store) or
                    self.clock() - self._permission[2] > 1):
                self.invalidate()
                raise ProtocolError("Fresh same-port service confirmation is required for credentials.")
        if self.active and (self._carry or getattr(self.transport, "in_waiting", 0)):
            self.invalidate()
            raise ProtocolError("Unsolicited service bytes invalidate the session; probe again.")
        self.correlation = self.correlation % 0xffffffff + 1
        frame = encode_frame(self.correlation, body)
        start = self.clock()
        self._write(frame)
        result = self._receive(self.correlation, allow_plain=discovery, probe=frame if discovery else None)
        self.round_trips.append(self.clock() - start)
        return result

    def command(self, body):
        """Bounded read-only diagnostics; mutations use the journaled APIs."""
        if body not in {"svc.stats", "svc.wifi.status", "svc.active"}:
            raise ServiceClientError("Use the journaled operation API for service mutations.")
        if not self.active:
            raise ProtocolError("A fresh service binding is required.")
        try:
            result = self._exchange(body)
            if body == "svc.active":
                _keys(result, {"ok", "store", "active"})
                active = result["active"]
                if active is not None:
                    _keys(active, {"op", "kind", "state"})
                    if (not _hex(active["op"], False) or not isinstance(active["kind"], str) or
                            not isinstance(active["state"], str) or active["kind"] not in {"schedule", "wifi"} or
                            active["state"] not in ({"staging", "committing"} if active["kind"] == "schedule" else {"connecting", "persisting"})):
                        raise ProtocolError("Invalid active operation metadata.")
            elif body == "svc.stats":
                counters = {"free_heap", "largest_block", "min_heap", "cli_stack", "worker_stack", "runner_stack",
                            "dropped_bytes", "dropped_calls", "logical_bytes", "records", "log_buffer_bytes"}
                extra = {"min_free_heap", "min_largest_block", "samples", "fs_total", "fs_used"}
                _keys(result, {"ok", "store"} | counters, extra)
                if extra & result.keys() and (not extra <= result.keys() or not all(_integer(result[key], 0, 0xffffffff) for key in extra)):
                    raise ProtocolError("Invalid sampled service budget counters.")
                if not all(_integer(result[key], 0, 0xffffffff) for key in counters) or result["logical_bytes"] > 32768 or result["records"] > 10 or result["log_buffer_bytes"] != 0:
                    raise ProtocolError("Invalid service budget counters.")
            else:
                _keys(result, {"ok", "store", "configured_ssid", "trial_ssid", "trial", "connected", "provisioned"})
                configured = result["configured_ssid"]
                if (not isinstance(configured, str) or "\0" in configured or len(configured.encode("utf-8")) > 32 or
                        any(type(result[k]) is not bool for k in ("trial", "connected", "provisioned")) or
                        (not _ssid(result["trial_ssid"]) if result["trial"] else result["trial_ssid"] is not None)):
                    raise ProtocolError("Invalid nonsecret Wi-Fi status.")
            return result
        except (TransportError, UnicodeError):
            self.invalidate()
            raise ProtocolError("Invalid service diagnostic response.") from None

    def sensors(self, timeout=45.0):
        """Reassemble one immutable read-only pin; no operation journal write."""
        from .sensors import decode_sensors, SensorsError
        previous = self._deadline
        self._deadline = min(self.clock() + min(45.0, max(0.1, timeout)), previous if previous is not None else float("inf"))
        try:
            if not self.active:
                self.connect()
            if not {"svc.sensors", "svc.sensors.read"} <= self.actions:
                raise ServiceClientError("Attached-sensor inspection is unavailable on this firmware.")
            head = self._exchange("svc.sensors")
            _keys(head, {"ok", "store", "snapshot", "size", "sha256", "chunk"})
            if (not _hex(head["snapshot"], False) or not _integer(head["size"], 1, 16384) or
                    type(head["chunk"]) is not int or head["chunk"] != 576 or
                    not isinstance(head["sha256"], str) or not SHA.fullmatch(head["sha256"])):
                raise ProtocolError("Invalid sensor snapshot descriptor.")
            data = bytearray()
            while len(data) < head["size"]:
                offset = len(data)
                page = self._exchange(f"svc.sensors.read {head['snapshot']} {offset:08x}")
                _keys(page, {"ok", "store", "snapshot", "offset", "crc32", "data", "eof"})
                if (page["snapshot"] != head["snapshot"] or page["offset"] != f"{offset:08x}" or
                        type(page["eof"]) is not bool or not isinstance(page["data"], str) or
                        len(page["data"]) > 768 or not isinstance(page["crc32"], str) or
                        not re.fullmatch(r"[0-9a-f]{8}", page["crc32"])):
                    raise ProtocolError("Invalid sensor snapshot page.")
                try:
                    chunk = base64.b64decode(page["data"], validate=True)
                except ValueError:
                    raise ProtocolError("Invalid sensor snapshot bytes.") from None
                count = min(576, head["size"] - offset)
                if (len(chunk) != count or base64.b64encode(chunk).decode() != page["data"] or
                        zlib.crc32(chunk) != int(page["crc32"], 16) or page["eof"] != (offset + count == head["size"])):
                    raise ProtocolError("Sensor snapshot page integrity check failed.")
                data.extend(chunk)
            if hashlib.sha256(data).hexdigest() != head["sha256"]:
                raise ProtocolError("Sensor snapshot digest mismatch.")
            try:
                return decode_sensors(bytes(data))
            except SensorsError as exc:
                raise ProtocolError(str(exc)) from exc
        except ServiceClientError:
            self.invalidate()
            raise
        finally:
            self._deadline = previous

    def _version(self, result):
        _keys(result, {"ok", "store", "service", "actions", "max_line", "chunk", "result_slots", "highest_id"})
        if (not _hex(result["store"], False) or type(result["service"]) is not int or result["service"] != 1 or
                type(result["max_line"]) is not int or result["max_line"] != LINE_MAX or
                type(result["chunk"]) is not int or result["chunk"] != CHUNK or
                type(result["result_slots"]) is not int or result["result_slots"] != 8 or
                not isinstance(result["actions"], list) or not all(isinstance(a, str) for a in result["actions"]) or
                len(set(result["actions"])) != len(result["actions"]) or not set(ACTION_LIST) <= set(result["actions"]) or
                not _hex(result["highest_id"])):
            raise ProtocolError("Advertised service version, actions or bounds are incompatible.")
        self.store, self.highest = result["store"], int(result["highest_id"], 16)
        self.actions = frozenset(result["actions"])

    def connect(self, *, new_request=False):
        self._configure_transport()
        self.invalidate()
        self.store = None
        try:
            try:
                version = self._exchange("svc.version", discovery=True)
            except PlainConsole:
                self._write(b"svc\n")
                deadline, setup_bytes = self._limit(), 0
                while True:
                    line = self._line(deadline)
                    setup_bytes += len(line)
                    if setup_bytes > 8192:
                        raise ProtocolError("Service entry produced too much plain output.")
                    try:
                        identifier, ready = decode_frame(line)
                    except PlainConsole:
                        continue
                    # Entry can have ordinary echoed/log lines, bounded above;
                    # never strip a prefix or ignore a malformed framed line.
                    except ProtocolError:
                        if re.match(rb"[0-9a-f]{8} ", line):
                            raise
                        continue
                    _keys(ready, {"ok", "store", "service", "state"})
                    if identifier != 0 or not ready["ok"] or type(ready["service"]) is not int or ready["service"] != 1 or ready["state"] != "ready":
                        raise ProtocolError("Invalid service ready marker.")
                    break
                version = self._exchange("svc.version")
                if ready["store"] != version["store"]:
                    raise ProtocolError("Store changed during service entry.")
            self._version(version)
            # Session RAM can be empty after reboot/reformat. Do not bind the
            # newly discovered instance on behalf of a previous-store intent.
            pending = self.journal.load() if self.journal is not None else None
            if pending and pending["phase"] != "complete" and pending["store"] != self.store and not new_request:
                raise UnknownOutcome("Device service store changed; verify the saved operation before a new request.")
            bound = self._exchange(f"svc.bind {self.store}")
            _keys(bound, {"ok", "store", "state"})
            if bound["state"] != "bound":
                raise ProtocolError("Service did not confirm the store binding.")
            self.active = True
            self._permission = (self.generation, self.store, self.clock())
        except ServiceClientError:
            self.invalidate()
            raise

    def exit(self):
        try:
            if self.active and (self._deadline is None or self.clock() < self._deadline):
                result = self._exchange("svc.exit")
                _keys(result, {"ok", "store", "state"})
                if result["state"] != "exit":
                    raise ProtocolError("Invalid service exit response.")
        finally:
            self.invalidate()
            for name, value in self._saved_settings.items():
                try:
                    setattr(self.transport, name, value)
                except Exception:
                    pass  # a vanished handle remains the caller's close responsibility

    def result(self, operation: str):
        if not self.active or not _hex(operation, False):
            raise ProtocolError("A fresh service binding and operation ID are required.")
        try:
            return self._result(self._exchange(f"svc.result {operation}"), operation)
        except TransportError:
            self.invalidate()
            raise

    def _result(self, result, operation):
        base = {"ok", "store", "op", "kind", "state", "reason", "offset", "size"}
        if result.get("kind") == "schedule":
            _keys(result, base | {"sha256", "workbook", "version", "built_fw", "applied"}, {"timed_out"})
            valid = (isinstance(result["sha256"], str) and SHA.fullmatch(result["sha256"]) and
                     (result["workbook"] is None or isinstance(result["workbook"], str) and UUID.fullmatch(result["workbook"])) and
                     isinstance(result["version"], str) and TOKEN.fullmatch(result["version"]) and
                     isinstance(result["built_fw"], str) and TOKEN.fullmatch(result["built_fw"]) and _truth(result["applied"]) and
                     ("timed_out" not in result or type(result["timed_out"]) is bool) and _integer(result["size"], 1, 16384))
        elif result.get("kind") == "wifi":
            _keys(result, base | {"ssid", "saved", "connected", "retrying", "restored", "restoration_connected"})
            valid = (_ssid(result["ssid"]) and
                     _truth(result["saved"]) and _truth(result["connected"]) and
                     all(type(result[k]) is bool for k in ("retrying", "restored", "restoration_connected")) and result["size"] == 0)
        else:
            raise ProtocolError("Invalid operation kind.")
        if (not valid or result["op"] != operation or not isinstance(result["state"], str) or result["state"] not in STATES or not _reason(result["reason"]) or
                not _integer(result["size"], 0, 16384) or not _integer(result["offset"], 0, result["size"])):
            raise ProtocolError("Invalid operation result descriptor or evidence.")
        return result

    @staticmethod
    def _matches(entry, result):
        return all(result.get(key) == value for key, value in entry["descriptor"].items())

    def _read_intent(self, entry):
        if entry["store"] != self.store:
            raise UnknownOutcome("Device service store changed; verify device state before an explicit new request.")
        try:
            result = self.result(entry["op"])
        except ServiceError as error:
            if error.reason == "not_found" and int(entry["op"], 16) > self.highest:
                return None
            if error.reason in {"not_found", "expired"}:
                raise UnknownOutcome("Saved operation outcome is unknown or expired; verify device state before retrying.") from None
            raise
        if not self._matches(entry, result):
            if result["state"] not in TERMINAL:
                raise BusyOperation(result["kind"], result["state"])
            raise UnknownOutcome("Saved operation descriptor conflicts with the device; nothing was changed.")
        return result

    def _foreign(self):
        if "svc.active" in self.actions:
            active = self.command("svc.active")["active"]
            if active is not None:
                result = self.result(active["op"])
                if (result["kind"], result["state"]) != (active["kind"], active["state"]):
                    # Work may finish between the two read-only exchanges.
                    if result["kind"] != active["kind"] or result["state"] not in TERMINAL:
                        raise ProtocolError("Active operation and result disagree.")
                if result["state"] not in TERMINAL:
                    raise BusyOperation(result["kind"], result["state"], result["op"])
            return
        if self.highest:
            try:
                latest = self.result(f"{self.highest:016x}")
            except ServiceError as error:
                if error.reason in {"not_found", "expired"}:
                    return
                raise
            if latest["state"] not in TERMINAL:
                raise BusyOperation(latest["kind"], latest["state"])

    def _intent(self, descriptor, *, new_request=False):
        previous = self.journal.load()
        if previous and previous["store"] != self.store and not new_request:
            raise UnknownOutcome("Device service store changed; verify device state before an explicit new request.")
        if previous and previous["phase"] != "complete" and not new_request:
            if previous["descriptor"] != descriptor:
                result = self._read_intent(previous)
                raise ServiceClientError(f"The saved {previous['descriptor']['kind']} operation ({result['state'] if result else 'reserved'}) must be resumed or cancelled first.")
            return previous
        self._foreign()
        highest = max(self.highest, int(previous["op"], 16) if previous and previous["store"] == self.store else 0)
        if highest == MAX_ID:
            raise ServiceError("id_exhausted")
        if previous:
            # An explicit restart may abandon our local intent only after the
            # device says it is terminal; it never cancels someone else's work.
            if previous["store"] == self.store and previous["phase"] != "complete":
                try:
                    result = self._read_intent(previous)
                except UnknownOutcome:
                    # new_request is an explicit caller decision after device
                    # state verification. _foreign already ruled out a live
                    # operation, including when this older ID has expired.
                    result = None
                if result and result["state"] not in TERMINAL:
                    raise BusyOperation(result["kind"], result["state"])
            self.journal.preserve()
        entry = dict(schema=1, mac=self.journal.mac, store=self.store, op=f"{highest + 1:016x}",
                     descriptor=descriptor, phase="reserved")
        self.journal.save(entry)
        return entry

    def _phase(self, entry, phase):
        entry["phase"] = phase
        self.journal.save(entry)

    def _finish(self, entry, result):
        if not self._matches(entry, result):
            raise UnknownOutcome("Device result does not match the saved transfer descriptor.")
        if result["state"] == "succeeded":
            if result["kind"] == "wifi":
                if result["saved"] is not True or result["connected"] is not True or result["reason"] or result["restoration_connected"]:
                    raise ProtocolError("Wi-Fi success lacks saved credentials and observed connection evidence.")
            elif (result["offset"] != result["size"] or
                  result["applied"] is False and result["reason"] != "already_active" or
                  result["applied"] is not False and result["reason"]):
                raise ProtocolError("Schedule success lacks complete activation evidence.")
        self._phase(entry, "uncertain" if result["state"] == "uncertain" else "complete")
        if result["state"] != "succeeded":
            if result["state"] == "uncertain":
                raise UnknownOutcome(f"Device outcome is uncertain ({result['reason'] or 'unprovable'}); verify device state.")
            raise ServiceError(result["reason"] or result["state"])
        return result

    def _recover(self, run, *, new_request=False):
        attempts = 0
        while self.clock() < self._deadline:
            try:
                if not self.active:
                    self.connect(new_request=new_request)
                return run()
            except TransportError:
                self.invalidate()
                if not self.reconnect or attempts == 2 or self.clock() >= self._deadline:
                    raise
                attempts += 1
                try:
                    replacement = self.reconnect(self._deadline - self.clock())
                except Exception:
                    raise TransportError("USB reconnection failed; the saved transfer can be resumed later.") from None
                self.replace_transport(replacement)
        raise UnknownOutcome("Transfer deadline reached; the saved operation can be read after reconnecting.")

    def _admission(self, body):
        try:
            return self._exchange(body)
        except ServiceError as error:
            if error.reason == "busy":
                # Another client may win between discovery and admission.
                # Refresh read-only metadata without changing our reserved ID.
                self.connect()
                self._foreign()
            raise

    def _poll(self, entry, cancelled):
        while self.clock() < self._deadline:
            if cancelled():
                raise TransferCancelled("Stopped waiting; the device operation continues and its intent is saved.")
            result = self._read_intent(entry)
            if result is None:
                raise UnknownOutcome("No admitted result is available; verify state before an explicit retry.")
            if result["state"] in TERMINAL:
                return self._finish(entry, result)
            self.sleep(min(0.2, max(0, self._deadline - self.clock())))
        raise UnknownOutcome("Device operation is still pending; its intent is saved for recovery.")

    @_journalled
    def abort(self, entry):
        owned = self.journal.load()
        if not owned or not isinstance(entry, dict) or any(owned.get(key) != entry.get(key) for key in ("mac", "store", "op", "descriptor")):
            raise ServiceClientError("No matching owned staging intent is available to cancel.")
        result = self._read_intent(entry)
        if result and result["state"] == "aborted" and result["kind"] == "schedule":
            self._phase(entry, "complete")
            return
        if result is None or result["state"] != "staging" or result["kind"] != "schedule":
            raise ServiceError("commit_started" if result and result["state"] in {"committing", "persisting", "connecting"} else "operation_complete")
        ack = self._exchange(f"svc.abort {entry['op']}")
        _keys(ack, {"ok", "store", "op", "state", "reason"})
        if ack["op"] != entry["op"] or ack["state"] != "aborted" or ack["reason"] != "aborted":
            raise ProtocolError("Invalid abort acknowledgement.")
        self._phase(entry, "complete")

    @_journalled
    def upload_schedule(self, blob: bytes, sha256: str, version: str, built_fw: str,
                        workbook: str | None = None, *, cancelled=lambda: False, log=None,
                        new_request=False, deadline_s=360.0, stage_only=False, on_staged=None):
        descriptor = schedule_descriptor(blob, sha256, version, built_fw, workbook)
        self._deadline = self.clock() + deadline_s
        entry = None
        startup_seen = False

        def transfer():
            nonlocal entry, startup_seen
            if entry is None:
                entry = self._intent(descriptor, new_request=new_request)
            result = self._read_intent(entry)
            if result is None:
                d = descriptor
                ack = self._admission(f"schedule.begin {entry['op']} {d['size']:08x} {d['sha256']} {d['version']} {d['built_fw']} {d['workbook'] or '-'}")
                _keys(ack, {"ok", "store", "op", "state", "offset"})
                if ack["op"] != entry["op"] or ack["state"] not in STATES or not _integer(ack["offset"], 0, len(blob)):
                    raise ProtocolError("Invalid schedule admission acknowledgement.")
                self._phase(entry, "admitted")
                result = self._read_intent(entry)
                if result is None:
                    raise ProtocolError("Admitted operation has no durable result.")
            if result["state"] in TERMINAL:
                if stage_only:
                    raise ServiceClientError("The saved staging measurement is terminal; use an explicit new request after readback.")
                return self._finish(entry, result)
            if result["state"] != "staging":
                if stage_only:
                    raise ServiceClientError("A staging measurement cannot take over a committed operation.")
                return self._poll(entry, cancelled)
            offset = result["offset"]
            if stage_only and offset:
                raise ServiceClientError("A staging measurement requires a fresh zero-byte prefix; recover the saved upload first.")
            if offset != len(blob) and offset % CHUNK:
                raise ProtocolError("Device upload offset is not a chunk boundary.")
            if log and offset:
                log(f"Resuming verified schedule at {offset}/{len(blob)} durable bytes.")
            while offset < len(blob):
                if cancelled():
                    self.abort(entry)
                    raise TransferCancelled("Schedule transfer cancelled before commit.")
                chunk = blob[offset:offset + CHUNK]
                encoded = base64.b64encode(chunk).decode("ascii")
                ack = self._exchange(f"p {entry['op']} {offset:08x} {zlib.crc32(chunk):08x} {encoded}")
                _keys(ack, {"ok", "store", "op", "state", "offset"})
                if ack["op"] != entry["op"] or ack["state"] != "staging" or type(ack["offset"]) is not int or ack["offset"] != offset + len(chunk):
                    raise ProtocolError("Device chunk acknowledgement disagrees with the durable prefix.")
                offset = ack["offset"]
                if log:
                    log(f"Pushed {offset}/{len(blob)} durable bytes of {version}.")
            if cancelled():
                self.abort(entry)
                raise TransferCancelled("Schedule transfer cancelled before commit.")
            if stage_only:
                # Bench real-fsync measurement uses exactly the GUI's p path.
                # Record its end before cleanup, then explicitly abort our own
                # verified staging intent. Never admit install/activation.
                if on_staged:
                    on_staged()
                self.abort(entry)
                result = self._read_intent(entry)
                if result is None or result["state"] != "aborted" or result["offset"] != len(blob):
                    raise ProtocolError("Staging measurement cleanup is not confirmed.")
                return result
            self._phase(entry, "committing")
            while self.clock() < self._deadline:
                if cancelled():
                    self.abort(entry)
                    raise TransferCancelled("Schedule transfer cancelled before commit.")
                try:
                    ack = self._exchange(f"schedule.commit {entry['op']}")
                except ServiceError as error:
                    if error.reason != "booting":
                        raise
                    # This explicit refusal guarantees the operation is still
                    # staging. Keep its ID, bytes and original deadline; never
                    # reinterpret a terminal busy or transport loss as booting.
                    if not startup_seen:
                        if log:
                            log("Device startup is still running; waiting to commit the staged schedule...")
                        startup_seen = True
                    self.sleep(min(0.2, max(0, self._deadline - self.clock())))
                    continue
                # The immediate reply is not completion evidence. Read the full
                # stored result even when commit returned a terminal result.
                self._result(ack, entry["op"])
                return self._poll(entry, cancelled)
            if startup_seen:
                raise ServiceClientError("Device startup did not finish before the transfer deadline; "
                                         "the staged schedule is saved and can be resumed after startup.")
            raise UnknownOutcome("Transfer deadline reached; the saved operation can be read after reconnecting.")

        try:
            return self._recover(transfer, new_request=new_request)
        finally:
            try:
                self.exit()
            except ServiceClientError:
                pass
            self._deadline = None

    @_journalled
    def join_wifi(self, ssid: str, password: str, *, cancelled=lambda: False,
                  new_request=False, deadline_s=60.0):
        try:
            ssid_bytes, password_bytes = ssid.encode("utf-8"), password.encode("ascii")
        except (AttributeError, UnicodeError):
            raise ServiceClientError("Invalid Wi-Fi credential format.") from None
        if (not 1 <= len(ssid_bytes) <= 31 or b"\0" in ssid_bytes or
                len(password_bytes) > 63 or any(c < 32 or c > 126 for c in password_bytes) or
                0 < len(password_bytes) < 8):
            raise ServiceClientError("Invalid Wi-Fi credential length or format.")
        self._deadline = self.clock() + deadline_s
        try:
            # No automatic credential retry, including a second invocation with
            # a different password while an earlier outcome remains unresolved.
            self.connect(new_request=new_request)
            previous = self.journal.load()
            if previous and previous["phase"] != "complete" and not new_request:
                raise UnknownOutcome("A saved operation must be recovered by ID before another credential attempt.")
            entry = self._intent(dict(kind="wifi", ssid=ssid), new_request=new_request)
            if cancelled():
                raise TransferCancelled("Wi-Fi trial cancelled before admission.")
            self._phase(entry, "admitted")
            if self._permission is None or self._permission[:2] != (self.generation, self.store) or self.clock() - self._permission[2] > 1:
                self.connect()
            if self._permission is None or self._permission[:2] != (self.generation, entry["store"]) or self.clock() - self._permission[2] > 1:
                raise ProtocolError("Fresh same-port service confirmation is required for credentials.")
            encoded_ssid = base64.b64encode(ssid_bytes).decode("ascii")
            encoded_password = base64.b64encode(password_bytes).decode("ascii") if password_bytes else "-"
            if cancelled():
                raise TransferCancelled("Wi-Fi trial cancelled before admission.")
            try:
                self._result(self._admission(f"svc.wifi.join {entry['op']} {encoded_ssid} {encoded_password}"), entry["op"])
            except TransportError:
                self.invalidate()
                # Recovery is read-only. The join body is never dispatched again.
            return self._recover(lambda: self._poll(entry, cancelled))
        finally:
            try:
                self.exit()
            except ServiceClientError:
                pass
            self._deadline = None

    @_journalled
    def recover_wifi(self, *, cancelled=lambda: False, deadline_s=60.0):
        entry = self.journal.load()
        if not entry or entry["descriptor"]["kind"] != "wifi":
            raise ServiceClientError("No saved Wi-Fi operation is available.")
        self._deadline = self.clock() + deadline_s
        try:
            return self._recover(lambda: self._poll(entry, cancelled))
        finally:
            try:
                self.exit()
            except ServiceClientError:
                pass
            self._deadline = None

"""Deterministic byte-level service peer; production C is tested separately."""
import base64
import copy
import json
import zlib

from flash_gui.service_client import ACTION_LIST

STORE = "0123456789abcdef"


def response(correlation, value):
    prefix = f"{correlation:08x} ".encode() + json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()
    return prefix + b"*%08x\n" % zlib.crc32(prefix)


class Clock:
    def __init__(self): self.value = 0.0
    def __call__(self): return self.value
    def sleep(self, seconds): self.value += seconds


class Device:
    def __init__(self):
        self.store = STORE
        self.highest = 0
        self.records = {}
        self.data = b""
        self.mode = False
        self.bound = False
        self.commits = self.applies = self.joins = self.aborts = 0
        self.polls = 0
        self.finish_after = 2
        self.before_admit = None
        self.outcome = "succeeded"
        self.error_reason = "compile_failed"

    def answer(self, body):
        a = body.split(" ")
        action = a[0]
        result = dict(ok=True, store=self.store)
        if action == "svc.version":
            return dict(result, service=1, actions=list(ACTION_LIST), max_line=248, chunk=144, result_slots=8,
                        highest_id=f"{self.highest:016x}")
        if action == "svc.bind":
            self.bound = a[1] == self.store
            return dict(result, state="bound") if self.bound else dict(result, ok=False, reason="store_changed")
        if action == "svc.exit":
            self.mode = self.bound = False
            return dict(result, state="exit")
        if not self.bound:
            return dict(result, ok=False, reason="store_changed")
        op = a[1]
        existing = self.records.get(op)
        if action == "svc.result":
            if existing is None:
                return dict(result, ok=False, reason="expired" if int(op, 16) <= self.highest else "not_found")
            if existing["state"] in {"committing", "connecting"}:
                self.polls += 1
                if self.polls >= self.finish_after:
                    existing["state"] = self.outcome
                    existing["reason"] = "" if self.outcome == "succeeded" else self.error_reason
            return dict(result, **copy.deepcopy(existing))
        if action in {"schedule.begin", "svc.wifi.join"}:
            if self.before_admit:
                self.before_admit(a)
            if existing:
                if action == "schedule.begin":
                    return dict(result, op=op, state=existing["state"], offset=existing["offset"])
                return dict(result, **copy.deepcopy(existing))
            if any(r["state"] not in {"succeeded", "failed", "uncertain", "aborted"} for r in self.records.values()):
                return dict(result, ok=False, reason="busy")
            if int(op, 16) <= self.highest:
                return dict(result, ok=False, reason="expired")
            self.highest = int(op, 16)
            if action == "schedule.begin":
                self.data = b""
                existing = dict(op=op, kind="schedule", state="staging", reason="", offset=0,
                                size=int(a[2], 16), sha256=a[3], version=a[4], built_fw=a[5],
                                workbook=None if a[6] == "-" else a[6], applied=False)
                self.records[op] = existing
                return dict(result, op=op, state="staging", offset=0)
            self.joins += 1
            existing = dict(op=op, kind="wifi", state="connecting", reason="", offset=0, size=0,
                            ssid=base64.b64decode(a[2]).decode(), saved=True, connected=True,
                            retrying=False, restored=False, restoration_connected=False)
            self.records[op] = existing
            return dict(result, **copy.deepcopy(existing))
        if existing is None:
            return dict(result, ok=False, reason="not_found")
        if action == "p":
            offset, crc, chunk = int(a[2], 16), int(a[3], 16), base64.b64decode(a[4], validate=True)
            assert zlib.crc32(chunk) == crc
            if offset == len(self.data): self.data += chunk
            else: assert self.data[offset:offset + len(chunk)] == chunk
            existing["offset"] = len(self.data)
            return dict(result, op=op, state="staging", offset=len(self.data))
        if action == "schedule.commit":
            self.commits += 1
            if existing["state"] == "staging":
                self.applies += 1
                existing["state"] = "committing"
                existing["applied"] = True
            return dict(result, **copy.deepcopy(existing))
        if action == "svc.abort":
            if existing["state"] == "committing": return dict(result, ok=False, reason="commit_started")
            self.aborts += 1
            existing["state"], existing["reason"] = "aborted", "aborted"
            return dict(result, op=op, state="aborted", reason="aborted")
        raise AssertionError(action)


class Serial:
    def __init__(self, device, clock, *, cut=None, mutate=None, raw=None, split=1024):
        self.device, self.clock = device, clock
        self.cut, self.mutate, self.raw, self.split = cut, mutate, raw, split
        self.counts, self.sent = {}, []
        self.output = bytearray()
        self.is_open = True
        self.timeout, self.write_timeout = 0.2, None

    @property
    def in_waiting(self): return len(self.output)

    def write(self, line):
        if not self.is_open: raise OSError("disconnected")
        assert self.write_timeout is not None and 0 < self.write_timeout <= 5
        self.sent.append(line)
        if line == b"svc\n":
            self.device.mode = True
            self.output.extend(b"svc\r\n" + response(0, dict(ok=True, store=self.device.store, service=1, state="ready")))
            return len(line)
        prefix, crc = line[:-1].rsplit(b"*", 1)
        assert zlib.crc32(prefix) == int(crc, 16)
        correlation, body = int(prefix[:8], 16), prefix[9:].decode()
        action = body.split(" ")[0]
        self.counts[action] = self.counts.get(action, 0) + 1
        hit = self.cut and self.cut[:2] == (action, self.counts[action])
        if hit and self.cut[2] == "before":
            self.is_open = False
            raise OSError("lost before device receive")
        if not self.device.mode:
            value = dict(ok=False, store=self.device.store, reason="mode_required")
            encoded = b"ambyte> " + response(0, value)
        else:
            value = self.device.answer(body)
            if self.mutate: value = self.mutate(action, value)
            encoded = response(correlation, value)
        if hit and self.cut[2] == "after":
            self.is_open = False
        else:
            if self.raw: encoded = self.raw(action, encoded)
            self.output.extend(encoded)
        return len(line)

    def read(self, size):
        if not self.is_open: raise OSError("disconnected")
        n = min(size, self.split, len(self.output))
        if not n: self.clock.sleep(0.1)
        data = bytes(self.output[:n]); del self.output[:n]
        return data

    def close(self): self.is_open = False

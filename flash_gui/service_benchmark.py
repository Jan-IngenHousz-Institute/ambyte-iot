# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Bench adapters over the shared production wire client; never GUI actions."""
from __future__ import annotations

import base64
import hashlib
import math
import re
import statistics
import zlib

from .service_client import ServiceClient, ServiceClientError, ProtocolError
from .service_wire import _keys

MIB = 1048576
BENCH_ACTIONS = {"bench.begin", "bench.read", "bench.end", "bench.abort"}
HEX8 = re.compile(r"[0-9a-f]{8}\Z")


def pattern(offset, size):
    return bytes(((offset + i) * 73 + 19) & 255 for i in range(size))


def rate_report(client, started, wire_before, samples, size):
    elapsed = client.clock() - started
    if not math.isfinite(elapsed) or elapsed <= 0 or not samples:
        raise ServiceClientError("Transfer clock did not advance; no rate can be reported.")
    ordered = sorted(samples)
    return dict(payload_bytes=size, elapsed_s=elapsed, effective_bytes_s=size / elapsed,
                wire_bytes_to_device=client.wire_written - wire_before[0],
                wire_bytes_from_device=client.wire_read - wire_before[1],
                frames=len(samples), rtt_median_s=statistics.median(samples),
                rtt_p95_s=ordered[math.ceil(.95 * len(ordered)) - 1])


class ServiceBenchmarkClient(ServiceClient):
    def transfer(self, direction, chunk, *, size=MIB, deadline_s=360.0):
        """One nonresumable checksum-sink run, bounded to 1 MiB.

        Begin/data/end RTTs and wire bytes are included; discovery and the
        before/after resource snapshots are excluded. A failed run is aborted
        if the same connection still works, never reconnected or resumed.
        """
        if (direction not in {"rx", "tx"} or type(size) is not int or not 1 <= size <= MIB or
                type(chunk) is not int or chunk not in ({144} if direction == "rx" else {144, 702})):
            raise ServiceClientError("Invalid bounded benchmark direction, size or chunk.")
        self._deadline = self.clock() + deadline_s
        admitted = False
        try:
            self.connect()
            if not BENCH_ACTIONS <= self.actions:
                raise ServiceClientError("This image has no transport benchmark; use the svc-bench build.")
            before = self.command("svc.stats")
            self.round_trips.clear()
            started, wire = self.clock(), (self.wire_written, self.wire_read)
            ready = self._exchange(f"bench.begin {direction} {size:08x} {chunk:08x}")
            _keys(ready, {"ok", "store", "state", "direction", "size", "chunk"})
            if (ready["state"] != "bench" or ready["direction"] != direction or
                    type(ready["size"]) is not int or ready["size"] != size or
                    type(ready["chunk"]) is not int or ready["chunk"] != chunk):
                raise ProtocolError("Invalid benchmark admission.")
            admitted = True
            digest, crc = hashlib.sha256(), 0
            for offset in range(0, size, chunk):
                data = pattern(offset, min(chunk, size - offset))
                if direction == "rx":
                    reply = self._exchange(f"p 0000000000000000 {offset:08x} {zlib.crc32(data):08x} " + base64.b64encode(data).decode())
                    _keys(reply, {"ok", "store", "op", "state", "offset"})
                    if (reply["op"] != "0000000000000000" or reply["state"] != "staging" or
                            type(reply["offset"]) is not int or reply["offset"] != offset + len(data)):
                        raise ProtocolError("Invalid checksum-sink chunk acknowledgement.")
                else:
                    reply = self._exchange(f"bench.read {offset:08x} {len(data):08x}")
                    _keys(reply, {"ok", "store", "offset", "crc32", "data"})
                    if (reply["offset"] != f"{offset:08x}" or not isinstance(reply["crc32"], str) or
                            not HEX8.fullmatch(reply["crc32"]) or not isinstance(reply["data"], str)):
                        raise ProtocolError("Invalid benchmark response metadata.")
                    try:
                        received = base64.b64decode(reply["data"], validate=True)
                    except (ValueError, UnicodeError):
                        raise ProtocolError("Invalid benchmark response data.") from None
                    if (received != data or base64.b64encode(received).decode() != reply["data"] or
                            zlib.crc32(received) != int(reply["crc32"], 16)):
                        raise ProtocolError("Benchmark response payload or checksum differs.")
                digest.update(data); crc = zlib.crc32(data, crc)
            final = self._exchange("bench.end")
            _keys(final, {"ok", "store", "state", "direction", "size", "offset", "chunk", "crc32"})
            if (final["state"] != "complete" or final["direction"] != direction or
                    any(type(final[key]) is not int for key in ("size", "offset", "chunk")) or
                    (final["size"], final["offset"], final["chunk"], final["crc32"]) != (size, size, chunk, f"{crc:08x}")):
                raise ProtocolError("Benchmark final checksum or count differs.")
            admitted = False
            report = rate_report(self, started, wire, list(self.round_trips), size)
            report.update(direction="ingress" if direction == "rx" else "egress", chunk_raw_bytes=chunk,
                          payload_sha256=digest.hexdigest(), crc32=f"{crc:08x}", store=self.store,
                          resources_before=before, resources_after=self.command("svc.stats"),
                          rtt_scope="begin, payload frames and end; excludes resource snapshots and discovery",
                          storage="checksum sink/generator; no payload writes, SD or OTA")
            return report
        finally:
            try:
                if admitted and self.active:
                    self._exchange("bench.abort")
            except ServiceClientError:
                pass
            try:
                self.exit()
            finally:
                self._deadline = None


def measure_stage(client, blob, version, built_fw, workbook=None, *, new_request=False):
    """Production 16 KiB p+fsync transfer, explicitly aborting before commit."""
    with client.journal.lease():
        return _measure_stage(client, blob, version, built_fw, workbook, new_request=new_request)


def _measure_stage(client, blob, version, built_fw, workbook, *, new_request):
    if len(blob) != 16384:
        raise ServiceClientError("The real-store measurement requires exactly 16,384 valid YAML bytes.")
    previous = client.journal.load()
    if previous and previous["phase"] != "complete" and not new_request:
        raise ServiceClientError("Recover or explicitly abort the saved measurement before timing a fresh full upload.")
    client.connect()
    resources = client.command("svc.stats")
    client.round_trips.clear()
    started, wire = client.clock(), (client.wire_written, client.wire_read)
    report = {}
    def staged():
        report.update(rate_report(client, started, wire, list(client.round_trips), len(blob)))
    result = client.upload_schedule(blob, hashlib.sha256(blob).hexdigest(), version, built_fw, workbook,
                                    stage_only=True, on_staged=staged, new_request=new_request)
    report.update(direction="ingress", chunk_raw_bytes=144, store=result["store"], op=result["op"],
                  cleanup=result["state"], payload_sha256=result["sha256"],
                  resources_before=resources, rtt_scope="admission/readback and p ACKs; excludes abort/exit/discovery",
                  storage="production stage file and durable offset fsync; no install, SD or OTA")
    client.connect()
    try:
        report["resources_after"] = client.command("svc.stats")
    finally:
        client.exit()
    return report


def projections(reports):
    rows = []
    for report in reports:
        rate = report.get("effective_bytes_s")
        if (type(rate) not in (int, float) or not math.isfinite(rate) or rate <= 0 or
                report.get("direction") not in {"ingress", "egress"} or
                report.get("chunk_raw_bytes") not in {144, 702} or not isinstance(report.get("storage"), str)):
            raise ServiceClientError("Projection input is not a completed measurement report.")
        rows.append(dict(direction=report["direction"], chunk_raw_bytes=report["chunk_raw_bytes"],
                         storage=report["storage"], effective_bytes_s=report["effective_bytes_s"],
                         backlog_9830400_s=9830400 / report["effective_bytes_s"],
                         image_2764800_s=2764800 / report["effective_bytes_s"]))
    return dict(method="linear payload/rate projections from measured runs; no full-size transfers",
                assumptions="Transport excludes flash/SD/OTA/export generation. Real-store includes small-file and offset fsync; neither predicts an OTA slot writer or SD export. A7/A9 must remeasure their storage adapters.",
                rows=rows)

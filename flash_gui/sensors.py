# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared strict attachment snapshot model for plain console and framed reads."""
from __future__ import annotations

import base64
from dataclasses import dataclass
import json
import math
import re
from types import MappingProxyType
from typing import Mapping
import zlib

MAX_BYTES = 16384
OBSERVATIONS = {"presence": "last_read", "sensor_id": "identity_cache",
                "firmware": "identity_cache", "hw_rev": "identity_cache",
                "calibration_digest": "identity_cache", "calibration": "last_read",
                "spectral": "last_read"}


class SensorsError(ValueError):
    pass


def _require(condition):
    if not condition:
        raise SensorsError("Invalid attached-sensor snapshot.")


def _integer(value, maximum=(1 << 63) - 1, minimum=0):
    return type(value) is int and minimum <= value <= maximum


def _number(value):
    return value is None or (type(value) in (int, float) and math.isfinite(value))


def _freeze(value):
    if isinstance(value, dict):
        return MappingProxyType({k: _freeze(v) for k, v in value.items()})
    if isinstance(value, list):
        return tuple(map(_freeze, value))
    return value


def _thaw(value):
    if isinstance(value, Mapping):
        return {k: _thaw(v) for k, v in value.items()}
    if isinstance(value, tuple):
        return list(map(_thaw, value))
    return value


@dataclass(frozen=True)
class SensorObservation:
    value: object
    source: str | None
    observed_uptime_ms: int | None
    observed_utc_ms: int | None
    clock: Mapping | None


@dataclass(frozen=True)
class SensorPort:
    port: int
    state: str
    generation: int
    stale: bool
    stale_reason: str | None
    age_ms: int | None
    observations: Mapping[str, SensorObservation]


@dataclass(frozen=True)
class SensorsSnapshot:
    boot_id: str
    captured_uptime_ms: int
    ports: tuple[SensorPort, ...]
    refresh: Mapping
    _raw: Mapping

    def as_dict(self):
        return _thaw(self._raw)


def _keys(obj, keys):
    _require(isinstance(obj, dict) and set(obj) == set(keys))


def _observation(obj, expected_source, captured):
    _keys(obj, ("value", "source", "observed_uptime_ms", "observed_utc_ms", "clock"))
    if obj["value"] is None:
        _require(all(v is None for v in obj.values()))
    else:
        _require(obj["source"] == expected_source)
        _require(_integer(obj["observed_uptime_ms"], captured))
        _require(obj["observed_utc_ms"] is None or _integer(obj["observed_utc_ms"], minimum=1704067200000))
        if obj["clock"] is not None:
            _keys(obj["clock"], ("source", "suspect"))
            _require(obj["clock"]["source"] in {"rtc", "hwm", "sntp"} and type(obj["clock"]["suspect"]) is bool)
    return SensorObservation(*(_freeze(obj[k]) for k in
        ("value", "source", "observed_uptime_ms", "observed_utc_ms", "clock")))


def _calibration(value):
    if value is None:
        return None
    floats = {"temp_offset", "temp_slope", "actinic_coef", "spec_coef", "mlx_emissivity", "sun_coef", "tick_factor"}
    acts = {f"act_{i}" for i in (50, 100, 150, 200, 250)}
    _keys(value, floats | acts | {"raw_base64", "ambit_name", "mlx_coef", "adpd"})
    _require(value["ambit_name"] is None or (isinstance(value["ambit_name"], str) and len(value["ambit_name"].encode("utf-8")) <= 20))
    _require(all(_number(value[k]) for k in floats) and all(_integer(value[k], 65535) for k in acts))
    _require(isinstance(value["mlx_coef"], list) and len(value["mlx_coef"]) == 14 and
             all(_integer(v, (1 << 31)-1, -(1 << 31)) for v in value["mlx_coef"]))
    _require(isinstance(value["adpd"], list) and len(value["adpd"]) == 6 and all(_integer(v, 0xffffffff) for v in value["adpd"]))
    _require(isinstance(value["raw_base64"], str))
    try:
        raw = base64.b64decode(value["raw_base64"], validate=True)
    except (ValueError, TypeError):
        raise SensorsError("Invalid calibration bytes.") from None
    _require(len(raw) == 140 and base64.b64encode(raw).decode() == value["raw_base64"])
    return raw


def parse_sensors(obj) -> SensorsSnapshot:
    try:
        _keys(obj, {"contract", "boot_id", "captured_uptime_ms", "refresh", "ports"})
        _require(type(obj["contract"]) is int and obj["contract"] == 1)
        _require(isinstance(obj["boot_id"], str) and re.fullmatch(r"[0-9a-f]{16}", obj["boot_id"]) and int(obj["boot_id"], 16) != 0)
        captured = obj["captured_uptime_ms"]
        _require(_integer(captured))
        _keys(obj["refresh"], {"wire_transactions", "deferred_acquisition", "run_start_waits", "run_start_wait_max_us",
            "stdout_hold_max_us", "pin_heap_bytes", "pin_heap_peak_bytes", "job_start_waits", "job_start_wait_max_us"})
        _require(isinstance(obj["refresh"]["wire_transactions"], list) and len(obj["refresh"]["wire_transactions"]) == 4)
        _require(all(_integer(v, 0xffffffff) for k, v in obj["refresh"].items() if k != "wire_transactions"))
        _require(all(_integer(v, 0xffffffff) for v in obj["refresh"]["wire_transactions"]))
        _require(isinstance(obj["ports"], list) and len(obj["ports"]) == 4)
        ports = []
        for index, port in enumerate(obj["ports"]):
            _keys(port, set(OBSERVATIONS) | {"port", "state", "generation", "stale", "stale_reason", "age_ms"})
            _require(type(port["port"]) is int and port["port"] == index)
            _require(port["state"] in {"known", "unknown", "absent", "busy"} and _integer(port["generation"], 0xffffffff))
            _require(type(port["stale"]) is bool and port["stale_reason"] in {None, "never_observed", "age", "revalidation_failed"})
            _require(port["stale"] == (port["stale_reason"] is not None))
            _require(port["age_ms"] is None or _integer(port["age_ms"]))
            observations = {k: _observation(port[k], source, captured) for k, source in OBSERVATIONS.items()}
            values = {k: observation.value for k, observation in observations.items()}
            _require(values["presence"] is None or type(values["presence"]) is bool)
            _require(values["sensor_id"] is None or (isinstance(values["sensor_id"], str) and re.fullmatch(r"[0-9A-F]{2}(?::[0-9A-F]{2}){5}", values["sensor_id"])))
            _require(values["firmware"] is None or (isinstance(values["firmware"], str) and re.fullmatch(r"\d{1,3}\.\d{1,3}\.\d{1,3}", values["firmware"]) and all(int(v) <= 255 for v in values["firmware"].split("."))))
            _require(values["hw_rev"] is None or _integer(values["hw_rev"], 255, 1))
            _require(values["sensor_id"] not in {"00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF"})
            _require((values["sensor_id"] is None) == (values["firmware"] is None))
            if port["state"] == "unknown":
                _require(values["sensor_id"] is None)
            if values["presence"] is None:
                _require(port["age_ms"] is None and port["stale_reason"] == "never_observed")
            else:
                _require(port["age_ms"] == captured - port["presence"]["observed_uptime_ms"])
                if port["stale_reason"] == "age":
                    _require(port["age_ms"] > 60000)
                if port["stale_reason"] is None:
                    _require(port["age_ms"] <= 60000)
            if port["state"] == "known":
                _require(values["sensor_id"] is not None and port["generation"] > 0)
            if port["state"] == "absent":
                _require(values["presence"] is False and all(values[k] is None for k in OBSERVATIONS if k != "presence"))
            raw = _calibration(port["calibration"]["value"])
            digest = port["calibration_digest"]["value"]
            _require((raw is None) == (digest is None))
            if digest is not None:
                _keys(digest, {"algorithm", "scope", "hex"})
                _require(digest["algorithm"] == "crc32-ieee" and digest["scope"] == "calibration_140")
                _require(digest["hex"] == f"{zlib.crc32(raw):08x}")
            spectral = port["spectral"]["value"]
            if spectral is not None:
                _keys(spectral, {"format", "spec_offset", "spec_sens", "par_weight", "par_slope", "par_intercept"})
                _require(type(spectral["format"]) is int and spectral["format"] == 1)
                for k in ("spec_offset", "spec_sens", "par_weight"):
                    _require(isinstance(spectral[k], list) and len(spectral[k]) == 10 and all(map(_number, spectral[k])))
                _require(_number(spectral["par_slope"]) and _number(spectral["par_intercept"]))
            ports.append(SensorPort(index, *(port[k] for k in ("state", "generation", "stale", "stale_reason", "age_ms")), MappingProxyType(observations)))
        return SensorsSnapshot(obj["boot_id"], captured, tuple(ports), _freeze(obj["refresh"]), _freeze(obj))
    except (TypeError, KeyError, UnicodeError, OverflowError) as exc:
        raise SensorsError("Invalid attached-sensor snapshot.") from exc


def decode_sensors(raw: bytes | str) -> SensorsSnapshot:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            _require(key not in result)
            result[key] = value
        return result
    try:
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", "strict")
        _require(len(raw.encode("utf-8")) <= MAX_BYTES and raw.startswith("{") and raw.endswith("}"))
        obj = json.loads(raw, object_pairs_hook=unique, parse_constant=lambda _: _require(False))
        return parse_sensors(obj)
    except (ValueError, UnicodeError) as exc:
        raise SensorsError("Invalid attached-sensor snapshot.") from exc

# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only operator diagnostics, also callable on the hardware bench."""
from dataclasses import dataclass
import json

from .ambyte_serial import connect_after_boot, PROBE_READY_TIMEOUT_S
from .sensors import SensorsSnapshot


@dataclass(frozen=True)
class SensorDiagnostics:
    snapshot: SensorsSnapshot | None
    message: str


def read_sensor_diagnostics(port, *, log=print) -> SensorDiagnostics:
    with connect_after_boot(port, deadline_s=PROBE_READY_TIMEOUT_S, log=log,
                            recover_service=True) as console:
        snapshot = console.sensors()
    if snapshot is None:
        return SensorDiagnostics(None, "Attached-sensor inspection is unavailable on this firmware.")
    return SensorDiagnostics(snapshot, "Last observed attachments; observations do not prove measurement or calibration validity.")


def diagnostic_rows(model: SensorDiagnostics):
    if model.snapshot is None:
        return ()
    rows = []
    for port in model.snapshot.ports:
        def observed(name):
            value = port.observations[name].value
            return "unavailable" if value is None else str(value)
        age = "unavailable" if port.age_ms is None else f"{port.age_ms / 1000:.1f} s"
        state = port.state + (f" — stale ({port.stale_reason})" if port.stale else "")
        rows.append((port.port, state, observed("sensor_id"), observed("firmware"), observed("hw_rev"), port.generation, age))
    return tuple(rows)


def diagnostic_details(model: SensorDiagnostics, port: int) -> str:
    if model.snapshot is None:
        return model.message
    return json.dumps(model.snapshot.as_dict()["ports"][port], ensure_ascii=False, indent=2)

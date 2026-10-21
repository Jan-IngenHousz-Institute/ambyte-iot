# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only operator diagnostics, also callable on the hardware bench."""
from dataclasses import dataclass
import json
import time

from .ambyte_serial import connect_after_boot, PROBE_READY_TIMEOUT_S, ConsoleError
from .sensors import SensorsSnapshot


@dataclass(frozen=True)
class SensorDiagnostics:
    snapshot: SensorsSnapshot | None
    message: str


def _observations_ready(snapshot):
    # Presence alone arrives before FW and calibration. Stay on this descriptor
    # briefly so opening/resetting the gateway does not force an empty readout.
    return all(port.observations["presence"].value is False or
               (port.observations["sensor_id"].value is not None and
                port.observations["calibration"].value is not None)
               for port in snapshot.ports)


def read_sensor_diagnostics(port, *, log=print) -> SensorDiagnostics:
    deadline = time.monotonic() + PROBE_READY_TIMEOUT_S
    snapshot = None
    with connect_after_boot(port, deadline_s=PROBE_READY_TIMEOUT_S, log=log,
                            recover_service=True) as console:
        observation_deadline = min(deadline, time.monotonic() + 10.0)
        while True:
            remaining = observation_deadline - time.monotonic()
            if remaining <= 0:
                if snapshot is None:
                    raise ConsoleError("Sensor inspection timed out after gateway startup.")
                break
            snapshot = console.sensors(timeout=min(5.0, remaining))
            if snapshot is None or _observations_ready(snapshot):
                break
            if time.monotonic() + .5 >= observation_deadline:
                break
            time.sleep(.5)
    if snapshot is None:
        return SensorDiagnostics(None, "Attached-sensor inspection is unavailable on this firmware.")
    observed = sum(p.observations["presence"].value is not None for p in snapshot.ports)
    if not observed:
        message = ("No attachments have been observed since this gateway boot. "
                   "Refresh waits for an idle window between measurement jobs.")
    elif not _observations_ready(snapshot):
        message = ("Some ports still lack identity or calibration observations since this gateway boot. "
                   "Refresh waits for an idle window; the rows show only the last observations.")
    else:
        message = "Last observed attachments; observations do not prove measurement or calibration validity."
    return SensorDiagnostics(snapshot, message)


def diagnostic_rows(model: SensorDiagnostics):
    if model.snapshot is None:
        return ()
    rows = []
    for port in model.snapshot.ports:
        def observed(name):
            value = port.observations[name].value
            return "unavailable" if value is None else str(value)
        age = "unavailable" if port.age_ms is None else f"{port.age_ms / 1000:.1f} s"
        state = ("stalled — completion unproven" if port.state == "stalled" else port.state)
        state += (f" — stale ({port.stale_reason})" if port.stale else "")
        rows.append((port.port, state, observed("sensor_id"), observed("firmware"), observed("hw_rev"), port.generation, age))
    return tuple(rows)


def diagnostic_details(model: SensorDiagnostics, port: int) -> str:
    if model.snapshot is None:
        return model.message
    return json.dumps(model.snapshot.as_dict()["ports"][port], ensure_ascii=False, indent=2)

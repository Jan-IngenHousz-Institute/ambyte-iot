# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""openJII API client for device provisioning (stdlib urllib only).

Authentication
--------------
The API's better-auth config exposes NO password login and no OAuth
device-code flow, and the browser session cookie is httpOnly + scoped to the
web origin — a desktop tool cannot harvest it. What better-auth DOES enable is
the apiKey plugin: a personal key (`jii_...`, created by the user in the web
UI under Platform → Account → API keys) sent as an `x-api-key` header
authenticates every /api/v1 endpoint. So "login" here is: open the API-keys
page in the user's browser, let them paste the key once, validate it against
/api/v1/auth/get-session, and keep it for the session.

Endpoints (packages/api/src/domains/... upstream)
-------------------------------------------------
  GET  /api/v1/auth/get-session                 200 + {user,...} | 200 + null
  GET  /api/v1/experiments?filter=member&status=active
  GET  /api/v1/devices                          this owner's devices
  POST /api/v1/devices                          register → Thing, status=pending
  POST /api/v1/devices/{id}/credentials         issue → SHOW-ONCE cert bundle
  POST /api/v1/devices/{id}/credentials/rotate  re-issue for a live device
  POST /api/v1/devices/{id}/onboard             bind + authoritative MQTT config

Issuance is refused while a certificate is live (status active/rotating) —
those go down the /rotate path. The returned private key is show-once: the
caller must write it to disk before any step that can fail.

All handlers are gated by the PostHog feature flag `iot-devices` per user; a
403 here almost always means that flag is off for the account.
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, replace
from pathlib import Path

from . import schedule_stamp
from .config import CERTS_DIR, OPENJII_DEVICE_TYPE, Environment
from .schedule_stamp import is_schedule_yaml
from .tls import ssl_context

USER_AGENT = "ambyte-flash-gui"

# States in which plain issuance is refused and /rotate is required.
LIVE_STATES = ("active", "rotating")


class OpenJIIError(RuntimeError):
    """API/auth failure, with a human-readable message."""


@dataclass
class Experiment:
    id: str
    name: str
    status: str


@dataclass(frozen=True)
class MacroCondition:
    """One device-evaluable routing condition of a macro's `when:` block.

    Compiled from a workbook branch path (see resolve_programming). `op` is
    the device spelling of the workbook's operator — only eq/neq survive the
    compile; ordering comparisons are rejected because the device cannot
    express them.
    """

    field: str
    op: str
    value: str


@dataclass(frozen=True)
class WorkbookMacro:
    """One macro cell of a workbook version, resolved to its pipeline identity.

    `filename` is the macro-sandbox module name the platform executes; it only
    exists on the persisted macro, not on the cell, so resolving it costs one
    GET /macros/{id} per cell. `when` is the routing compiled from the
    workbook's branch cells; empty means the macro applies to every row.
    """

    id: str
    name: str
    filename: str
    when: tuple[MacroCondition, ...] = ()

    def without_when(self) -> WorkbookMacro:
        """This macro with its routing stripped (the pre-when firmware gate)."""
        return replace(self, when=())


@dataclass(frozen=True)
class WorkbookProgramming:
    """The installable schedule found in an experiment's pinned workbook version.

    `yaml_text` is the programming cell's raw content — deliberately generic
    (no ids); whoever installs it stamps the header (see schedule_stamp).
    """

    yaml_text: str
    workbook_id: str
    workbook_version_id: str
    workbook_version_number: int | None
    macros: tuple[WorkbookMacro, ...]
    # False when branch routing was NOT compiled because the firmware this
    # resolve was made for predates MACRO_WHEN_MIN_FW (or was unknown). Then
    # every `macros[].when` is () for that reason, not because the workbook
    # declares no routing — procedure.schedule_source refuses to stamp such a
    # snapshot onto firmware that WOULD evaluate `when:`, since that would
    # silently publish every macro on every row.
    routing_compiled: bool = False


@dataclass
class DeviceIdentity:
    """What provisioning needs from the registry + credential issuance."""

    device_id: str
    thing_name: str        # MQTT client id MUST equal this
    certificate_pem: str
    private_key_pem: str
    bundle_dir: Path       # where the show-once PEMs were persisted
    rotated: bool


@dataclass(frozen=True)
class DeviceOnboarding:
    """Authoritative connection contract returned by openJII."""

    thing_name: str
    device_type: str
    endpoint: str
    topic_prefix: str


class OpenJIIClient:
    def __init__(self, env: Environment, api_key: str):
        self.env = env
        self.api_key = (api_key or "").strip()

    # ── HTTP plumbing ────────────────────────────────────────────────────
    def _request(self, method: str, path: str, body: dict | None = None,
                 timeout: int = 60):
        url = f"{self.env.api_url}{path}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        headers = {
            "User-Agent": USER_AGENT,
            "Accept": "application/json",
            "x-api-key": self.api_key,
        }
        if data is not None:
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers,
                                     method=method)
        try:
            with urllib.request.urlopen(
                    req, timeout=timeout, context=ssl_context()) as resp:
                raw = resp.read().decode("utf-8") or ""
                return resp.status, (json.loads(raw) if raw.strip() else None)
        except urllib.error.HTTPError as exc:
            raw = ""
            try:
                raw = exc.read().decode("utf-8", errors="replace")
            except Exception:
                pass
            try:
                parsed = json.loads(raw) if raw.strip() else None
            except Exception:
                parsed = {"raw": raw[:400]}
            return exc.code, parsed
        except urllib.error.URLError as exc:
            raise OpenJIIError(
                f"cannot reach {self.env.api_url} ({exc.reason}). "
                "Check the network/VPN.") from exc

    @staticmethod
    def _error_text(payload) -> str:
        if isinstance(payload, dict):
            for key in ("message", "error", "detail", "raw"):
                val = payload.get(key)
                if isinstance(val, str) and val:
                    return val
                if isinstance(val, dict) and isinstance(val.get("message"), str):
                    return val["message"]
        return json.dumps(payload)[:300] if payload is not None else "(no body)"

    # ── auth ─────────────────────────────────────────────────────────────
    def validate_key(self) -> dict:
        """The signed-in user object, or raises. better-auth answers 200 with
        a null body for an unauthenticated call — that is a failure here."""
        if not self.api_key:
            raise OpenJIIError("no API key set for this environment.")
        status, payload = self._request("GET", "/api/v1/auth/get-session",
                                        timeout=20)
        if status != 200 or not (isinstance(payload, dict) and payload.get("user")):
            raise OpenJIIError(
                "the API key was rejected (or expired). Create one at "
                f"{self.env.api_keys_url} and paste it again.")
        return payload["user"]

    # ── experiments ──────────────────────────────────────────────────────
    def list_experiments(self) -> list[Experiment]:
        """Active experiments the signed-in user is a member of."""
        query = urllib.parse.urlencode({"filter": "member", "status": "active"})
        status, payload = self._request("GET", f"/api/v1/experiments?{query}")
        if status != 200 or not isinstance(payload, list):
            raise OpenJIIError(
                f"listing experiments failed ({status}): "
                f"{self._error_text(payload)}")
        out = []
        for exp in payload:
            if isinstance(exp, dict) and exp.get("id"):
                out.append(Experiment(id=exp["id"],
                                      name=exp.get("name") or exp["id"],
                                      status=exp.get("status") or ""))
        return out

    # ── device registry ──────────────────────────────────────────────────
    def find_device(self, serial: str) -> dict | None:
        status, payload = self._request("GET", "/api/v1/devices")
        if status == 403:
            raise OpenJIIError(
                "openJII refused the device registry (403). The `iot-devices` "
                "feature flag is probably off for your account: "
                f"{self._error_text(payload)}")
        if status != 200 or not isinstance(payload, list):
            raise OpenJIIError(f"listing devices failed ({status}): "
                               f"{self._error_text(payload)}")
        for dev in payload:
            if isinstance(dev, dict) and dev.get("serialNumber") == serial:
                return dev
        return None

    def register_device(self, serial: str, name: str | None) -> dict:
        body: dict = {"serialNumber": serial, "deviceType": OPENJII_DEVICE_TYPE}
        if name:
            body["name"] = name
        status, payload = self._request("POST", "/api/v1/devices", body)
        if status == 201 and isinstance(payload, dict):
            return payload
        if status == 409:
            # Raced with another station, or the pre-check missed it.
            existing = self.find_device(serial)
            if existing:
                return existing
        raise OpenJIIError(f"registering {serial} failed ({status}): "
                           f"{self._error_text(payload)}")

    def issue_credentials(self, device: dict) -> dict:
        device_id = device.get("id")
        if not device_id:
            raise OpenJIIError("openJII returned a device without an id.")
        rotate = device.get("status") in LIVE_STATES
        path = (f"/api/v1/devices/{device_id}/credentials/rotate" if rotate
                else f"/api/v1/devices/{device_id}/credentials")
        status, payload = self._request("POST", path)
        if status == 201 and isinstance(payload, dict):
            return payload
        raise OpenJIIError(f"issuing credentials failed ({status}): "
                           f"{self._error_text(payload)}")

    def onboard_device(self, device_id: str,
                       experiment_id: str) -> DeviceOnboarding:
        """Bind a device to one experiment and return its server-owned config.

        The flasher does not consume workbook procedures, so it explicitly asks
        for the connection/topic contract only. The response can include all
        existing bindings; this single-topic firmware uses the selected one.
        """
        if not device_id:
            raise OpenJIIError("cannot onboard a device without an id.")
        if not experiment_id:
            raise OpenJIIError("cannot onboard a device without an experiment id.")

        path = f"/api/v1/devices/{device_id}/onboard"
        body = {"experimentIds": [experiment_id], "includeWorkbook": False}
        status, payload = self._request("POST", path, body)
        if status != 200 or not isinstance(payload, dict):
            raise OpenJIIError(f"onboarding device failed ({status}): "
                               f"{self._error_text(payload)}")

        experiments = payload.get("experiments")
        selected = next(
            (item for item in experiments
             if isinstance(item, dict) and item.get("experimentId") == experiment_id),
            None,
        ) if isinstance(experiments, list) else None

        thing_name = payload.get("thingName")
        device_type = payload.get("deviceType")
        endpoint = payload.get("endpoint")
        topic_prefix = selected.get("topicPrefix") if selected else None
        missing = [
            label for label, value in (
                ("thingName", thing_name),
                ("deviceType", device_type),
                ("endpoint", endpoint),
                (f"topicPrefix for experiment {experiment_id}", topic_prefix),
            ) if not isinstance(value, str) or not value.strip()
        ]
        if missing:
            raise OpenJIIError(
                "openJII returned an incomplete onboarding config: "
                + ", ".join(missing))

        return DeviceOnboarding(
            thing_name=thing_name.strip(),
            device_type=device_type.strip(),
            endpoint=endpoint.strip(),
            topic_prefix=topic_prefix.strip().strip("/"),
        )

    def list_experiment_devices(self, experiment_id: str) -> list[dict]:
        """Devices bound to an experiment, flattened to the device objects.

        GET /experiments/{id}/devices answers ``[{device: {...}, addedBy,
        addedAt}]``; only collaborators see it, and it is gated by the same
        `iot-devices` flag as the registry.
        """
        status, payload = self._request(
            "GET", f"/api/v1/experiments/{experiment_id}/devices")
        if status == 403:
            raise OpenJIIError(
                f"openJII refused the device list of {experiment_id} (403): "
                "you need collaborator access and the `iot-devices` flag: "
                f"{self._error_text(payload)}")
        if status != 200 or not isinstance(payload, list):
            raise OpenJIIError(
                f"listing devices of experiment {experiment_id} failed "
                f"({status}): {self._error_text(payload)}")
        devices = []
        for entry in payload:
            device = entry.get("device") if isinstance(entry, dict) else None
            if isinstance(device, dict):
                devices.append(device)
        return devices

    # ── workbook programming (schedule install source) ───────────────────
    def get_experiment(self, experiment_id: str) -> dict:
        """The experiment detail; carries the pinned workbookId/versionId."""
        status, payload = self._request(
            "GET", f"/api/v1/experiments/{experiment_id}")
        if status != 200 or not isinstance(payload, dict):
            raise OpenJIIError(f"reading experiment {experiment_id} failed "
                               f"({status}): {self._error_text(payload)}")
        return payload

    def get_workbook_version(self, workbook_id: str, version_id: str) -> dict:
        """One immutable workbook version, including its cells."""
        status, payload = self._request(
            "GET", f"/api/v1/workbooks/{workbook_id}/versions/{version_id}")
        if status != 200 or not isinstance(payload, dict):
            raise OpenJIIError(
                f"reading workbook version {version_id} failed ({status}): "
                f"{self._error_text(payload)}")
        return payload

    def get_macro(self, macro_id: str) -> dict:
        """The persisted macro; `filename` is what the pipeline executes."""
        status, payload = self._request("GET", f"/api/v1/macros/{macro_id}")
        if status != 200 or not isinstance(payload, dict):
            raise OpenJIIError(f"reading macro {macro_id} failed ({status}): "
                               f"{self._error_text(payload)}")
        return payload

    def resolve_programming(self, experiment_id: str, fw_version: str = "",
                            log=None) -> WorkbookProgramming | None:
        """The Ambyte schedule pinned to this experiment via its workbook.

        The programming cell is the version's command cell whose YAML carries a
        top-level ``schema: jii.ambyte-schedule/...`` line. Returns None — never
        raises — when the experiment has no pinned workbook version or the
        pinned version has no such cell; HTTP failures raise OpenJIIError.

        Per-macro routing is compiled from the version's BRANCH cells: a path
        whose ``gotoCellId`` is a macro cell becomes that macro's ``when:``
        block (see _compile_macro_routing). Workbook content the device cannot
        express — an unknown field, an ordering operator, or routing one macro
        from several paths — raises OpenJIIError rather than silently
        misrouting rows.

        `fw_version` is the firmware ABOUT TO BE FLASHED. Compilation happens
        only when that firmware evaluates `when:` at all: an unparseable or
        older version skips it entirely (fail-closed, same as every other
        header gate), so a fleet that will never read the routing cannot be
        blocked by a workbook whose branches this device cannot express.
        """
        experiment = self.get_experiment(experiment_id)
        workbook_id = experiment.get("workbookId")
        version_id = experiment.get("workbookVersionId")
        if not isinstance(workbook_id, str) or not workbook_id \
                or not isinstance(version_id, str) or not version_id:
            return None

        version = self.get_workbook_version(workbook_id, version_id)
        cells = version.get("cells")
        if not isinstance(cells, list):
            raise OpenJIIError(
                f"workbook version {version_id} returned no cell list.")

        yaml_text = None
        command_cell_id = None
        macros: list[WorkbookMacro] = []
        macro_index_by_cell: dict[str, int] = {}  # macro CELL id → macros index
        branch_cells: list[dict] = []
        for cell in cells:
            if not isinstance(cell, dict):
                continue
            if cell.get("type") == "branch":
                # NOT payload-wrapped: zBranchCell puts `paths` at the CELL top
                # level while zCommandCell/zMacroCell wrap theirs in `payload`
                # (open-jii packages/api/.../workbook-cells.schema.ts). Testing
                # the type BEFORE the payload guard below is load-bearing: the
                # guard used to drop every schema-conformant branch cell, which
                # made routing silently compile to "applies to every row".
                branch_cells.append(cell)
                continue
            payload = cell.get("payload")
            if not isinstance(payload, dict):
                continue
            if cell.get("type") == "command" and yaml_text is None:
                content = payload.get("content")
                if (payload.get("format") == "yaml"
                        and isinstance(content, str)
                        and is_schedule_yaml(content)):
                    yaml_text = content
                    command_cell_id = cell.get("id")
            elif cell.get("type") == "macro":
                macro_id = payload.get("macroId")
                if not isinstance(macro_id, str) or not macro_id:
                    continue
                macro = self.get_macro(macro_id)
                filename = macro.get("filename")
                if not isinstance(filename, str) or not filename:
                    raise OpenJIIError(
                        f"macro {macro_id} has no filename — the pipeline "
                        "cannot key its output table without it.")
                name = macro.get("name") or payload.get("name") or macro_id
                cell_id = cell.get("id")
                if isinstance(cell_id, str) and cell_id:
                    macro_index_by_cell[cell_id] = len(macros)
                macros.append(WorkbookMacro(id=macro.get("id") or macro_id,
                                            name=str(name),
                                            filename=filename))

        if yaml_text is None:
            return None
        if schedule_stamp.has_authored_macros_block(yaml_text):
            raise OpenJIIError(
                "the workbook's command cell carries an authored `macros:` "
                "block — routing comes from workbook branches; remove the "
                "macros: block from the command cell.")
        routing_compiled = schedule_stamp.firmware_supports_macro_when(
            fw_version)
        if routing_compiled:
            macros = _compile_macro_routing(branch_cells, macros,
                                            macro_index_by_cell,
                                            command_cell_id, log=log)
        elif log is not None and branch_cells:
            log(f"openJII: firmware {fw_version or 'unknown'} predates "
                f"{schedule_stamp.MACRO_WHEN_MIN_FW}, so the version's "
                f"{len(branch_cells)} branch cell(s) were NOT compiled into "
                "macro routing. Every macro will apply to every row.")
        if log is not None:
            routed = sum(1 for m in macros if m.when)
            log(f"openJII: workbook {workbook_id} version {version_id} has an "
                f"Ambyte programming cell ({len(macros)} macro(s), "
                f"{routed} with branch routing).")
        number = version.get("versionNumber", version.get("version"))
        return WorkbookProgramming(
            yaml_text=yaml_text,
            workbook_id=workbook_id,
            workbook_version_id=version_id,
            workbook_version_number=number if isinstance(number, int) else None,
            macros=tuple(macros),
            routing_compiled=routing_compiled,
        )

    # ── the full provisioning round ──────────────────────────────────────
    def provision_device(self, serial: str, name: str | None,
                         log=print) -> DeviceIdentity:
        """Register `serial` if needed, issue/rotate its cert, persist the
        show-once bundle to disk BEFORE returning."""
        device = self.find_device(serial)
        if device:
            log(f"openJII: {serial} already registered as "
                f"{device.get('thingName')}.")
        else:
            device = self.register_device(serial, name)
            log(f"openJII: registered {serial} as {device.get('thingName')}.")

        rotated = device.get("status") in LIVE_STATES
        if rotated:
            log(f"openJII: device has a live certificate "
                f"(status={device.get('status')}) — rotating.")
        creds = self.issue_credentials(device)

        thing_name = device.get("thingName")
        if not thing_name:
            raise OpenJIIError("openJII returned a device without a thingName.")
        cert = creds.get("certificatePem")
        key = creds.get("privateKey")
        if not cert or not key:
            raise OpenJIIError(
                "openJII returned no certificatePem/privateKey — cannot "
                "provision.")

        bundle = _write_bundle(thing_name, creds, log)
        return DeviceIdentity(device_id=device.get("id") or "",
                              thing_name=thing_name,
                              certificate_pem=cert,
                              private_key_pem=key,
                              bundle_dir=bundle,
                              rotated=rotated)


def _compile_macro_routing(branch_cells: list[dict],
                           macros: list[WorkbookMacro],
                           macro_index_by_cell: dict[str, int],
                           command_cell_id, log=None) -> list[WorkbookMacro]:
    """Fold the version's branch cells into per-macro `when:` blocks.

    A branch path whose ``gotoCellId`` names a macro cell of the pinned
    version becomes that macro's routing, provided its conditions all read
    the schedule COMMAND cell's output (that is what the publisher evaluates:
    the row being published). Paths routing anywhere else in the workbook,
    and paths reading another cell's output, are none of the device's
    business and are SKIPPED, because one workbook legitimately serves the
    mobile flow and the Ambyte at once and a shared macro cell must not
    hard-fail the Ambyte install. Macros no usable path targets keep
    ``when == ()``: they apply to every row, exactly as before this feature.

    ``defaultPathId`` is ignored ON PURPOSE and that is correct, not an
    oversight: the default path is itself a member of ``paths``, and with no
    conditions it compiles to ``when == ()`` = applies to every row.

    Content the device cannot express on a path it WOULD otherwise compile
    (unknown field, ordering operator, too many conditions) raises
    OpenJIIError naming the branch cell and path: a silently dropped or
    miscompiled condition misroutes rows, which is worse than failing.
    """
    whens: dict[int, tuple[MacroCondition, ...]] = {}  # macros index → routing
    for branch in branch_cells:
        branch_id = branch.get("id") or "(no id)"
        # zBranchCell keeps `paths` at the cell top level; the payload-wrapped
        # form is tolerated only so an older/hand-built cell still resolves.
        paths = branch.get("paths")
        if paths is None:
            payload = branch.get("payload")
            paths = payload.get("paths") if isinstance(payload, dict) else None
        if not isinstance(paths, list):
            raise OpenJIIError(
                f"branch cell {branch_id} has no paths list — cannot check "
                "its routing against the device contract.")
        for index, path in enumerate(paths):
            where = f"branch cell {branch_id} path {index}"
            if not isinstance(path, dict):
                raise OpenJIIError(f"{where} is not an object.")
            goto = path.get("gotoCellId")
            if goto not in macro_index_by_cell:
                continue  # routes to a non-macro cell: web-side control flow
            conditions = path.get("conditions") or []
            if not isinstance(conditions, list):
                raise OpenJIIError(f"{where} has a malformed conditions list.")
            # A path reading another cell's output is somebody else's flow.
            # Checked BEFORE the field/op validation so a mobile branch using
            # fields this device never sees cannot fail the install.
            foreign = next(
                (cond.get("sourceCellId") for cond in conditions
                 if isinstance(cond, dict)
                 and cond.get("sourceCellId") != command_cell_id), None)
            if foreign is not None:
                if log is not None:
                    log(f"openJII: {where} reads cell {foreign!r}, not the "
                        f"schedule command cell ({command_cell_id!r}); it is "
                        "another flow's routing and is not compiled for the "
                        "device.")
                continue
            if len(conditions) > schedule_stamp.MAX_MACRO_CONDITIONS:
                raise OpenJIIError(
                    f"{where} has {len(conditions)} conditions — the device "
                    f"expresses at most "
                    f"{schedule_stamp.MAX_MACRO_CONDITIONS} AND-ed conditions "
                    "per macro.")
            compiled = []
            for cond in conditions:
                if not isinstance(cond, dict):
                    raise OpenJIIError(f"{where} has a malformed condition.")
                field = cond.get("field")
                if field not in schedule_stamp.MACRO_WHEN_FIELDS:
                    raise OpenJIIError(
                        f"{where} condition field {field!r} is not "
                        "device-addressable (one of: "
                        f"{', '.join(sorted(schedule_stamp.MACRO_WHEN_FIELDS))}).")
                operator = cond.get("operator")
                if operator not in schedule_stamp.MACRO_WHEN_OPS:
                    raise OpenJIIError(
                        f"{where} condition operator {operator!r} cannot be "
                        "expressed on device (eq|neq only).")
                value = cond.get("value")
                if not isinstance(value, str):
                    raise OpenJIIError(
                        f"{where} condition value must be a string, got "
                        f"{value!r}.")
                compiled.append(MacroCondition(field=field, op=operator,
                                               value=value))
            target = macro_index_by_cell[goto]
            if target in whens:
                macro = macros[target]
                raise OpenJIIError(
                    f"macro '{macro.name}' ({macro.id}) is targeted by more "
                    "than one branch path — the device expresses a single "
                    "AND-ed condition set per macro, so web-side OR routing "
                    "is not representable. Rework the workbook branches.")
            whens[target] = tuple(compiled)
    return [replace(macro, when=whens.get(i, ()))
            for i, macro in enumerate(macros)]


def _write_bundle(thing_name: str, creds: dict, log=print) -> Path:
    """Persist the show-once PEMs under the config dir.

    Any previous bundle for this Thing is moved aside, not deleted: after a
    rotate the old key is dead, but if anything downstream goes wrong we would
    rather still have it on disk than have destroyed the only copy.
    """
    import os

    safe = thing_name.replace(":", "-")   # Windows forbids ':' in paths
    cert_id = creds.get("certificateId") or safe
    bundle = CERTS_DIR / safe
    if bundle.exists() and any(bundle.iterdir()):
        aside = bundle.with_name(f"{safe}.superseded.{int(time.time())}")
        bundle.rename(aside)
        log(f"Previous credentials kept at {aside.name}")
    bundle.mkdir(parents=True, exist_ok=True)

    (bundle / f"{cert_id}.cert.pem").write_text(
        creds["certificatePem"], encoding="utf-8")
    key_path = bundle / f"{cert_id}.private.key"
    key_path.write_text(creds["privateKey"], encoding="utf-8")
    try:
        os.chmod(key_path, 0o600)   # POSIX; Windows ACLs come from the profile
    except OSError:
        pass
    if creds.get("publicKey"):
        (bundle / f"{cert_id}.public.key").write_text(
            creds["publicKey"], encoding="utf-8")
    return bundle

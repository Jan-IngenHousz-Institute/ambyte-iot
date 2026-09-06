# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Schedule source selection and the workbook install path in the procedure.

The workbook path must thread one identity — sha256 of the STAMPED bytes —
through NVS provenance (step 3), the serial push (step 6) and the verify
read-back (step 7), and it must never silently fall back to a device-side
download: workbook bytes have no release URL.
"""

import base64
import hashlib
from types import SimpleNamespace

import pytest

from flash_gui import ambyte_serial, procedure, schedule_stamp
from flash_gui.ambyte_serial import AmbyteConsole, ScheduleReleaseStatus
from flash_gui.openjii_client import WorkbookMacro, WorkbookProgramming
from flash_gui.procedure import ProcedureError

VERSION_ID = "1c7b82b5-0000-1111-2222-333333333333"
MACRO = WorkbookMacro(id="47b03f78-1111-2222-3333-444455556666",
                      name="ambyte-trace", filename="macro_8feac276a118")

SCHEDULE_YAML = (
    "schema: jii.ambyte-schedule/v1-draft\n"
    "id: urn:jii:schedule:baseline\n"
    "description: SS every minute.\n"
    "jobs:\n"
    "  health:\n"
    "    schedule:\n"
    "      - boot\n"
    "    steps:\n"
    "      - uses: device/status-report\n"
)

CATALOG = SimpleNamespace(
    asset_name="default.yaml", sha256="a" * 64, script_version="1.2.0",
    built_against_fw="v1.8.1", campaign_id="schedule-v1.2.0",
    asset_url="https://example.test/default.yaml", size_bytes=6731)


def _programming(version_number=3, macros=(MACRO,)):
    return WorkbookProgramming(
        yaml_text=SCHEDULE_YAML, workbook_id="wb-id",
        workbook_version_id=VERSION_ID,
        workbook_version_number=version_number, macros=macros)


def _ctx(programming=None, fw="2.1.0"):
    return SimpleNamespace(schedule_script=CATALOG, programming=programming,
                           release=SimpleNamespace(version=fw),
                           wifi_ssid="bench-ap")


def _stamped(macros=(MACRO,)):
    return schedule_stamp.stamp_header(SCHEDULE_YAML, VERSION_ID, macros,
                                       checker=lambda _t: None)


# ── source selection ─────────────────────────────────────────────────────────
def test_catalog_source_is_the_default_and_mirrors_the_manifest():
    source = procedure.schedule_source(_ctx(), log=lambda _m: None)
    assert source.origin == "catalog"
    assert source.blob is None  # bytes keep coming from the digest-checked cache
    assert source.sha256 == CATALOG.sha256
    assert source.script_version == "1.2.0"
    assert source.campaign_id == "schedule-v1.2.0"
    assert source.asset_url == CATALOG.asset_url
    assert source.workbook_version_id is None


def test_workbook_source_identity_is_the_stamped_bytes():
    source = procedure.schedule_source(_ctx(programming=_programming()),
                                       log=lambda _m: None)
    assert source.origin == "workbook"
    assert source.blob == _stamped().encode("utf-8")
    assert source.sha256 == hashlib.sha256(source.blob).hexdigest()
    assert source.sha256 != hashlib.sha256(SCHEDULE_YAML.encode()).hexdigest()
    assert source.script_version == "wb-v3"
    assert source.campaign_id == f"workbook-{VERSION_ID}"
    assert source.built_against_fw == "2.1.0"   # the firmware being flashed
    assert source.asset_url is None
    assert source.workbook_version_id == VERSION_ID
    assert b"macros:" in source.blob


def test_workbook_source_without_version_number_falls_back_to_id_prefix():
    source = procedure.schedule_source(
        _ctx(programming=_programming(version_number=None)),
        log=lambda _m: None)
    assert source.script_version == f"wb-{VERSION_ID[:8]}"


def test_macros_are_gated_on_the_flashed_firmware():
    messages = []
    source = procedure.schedule_source(_ctx(programming=_programming(),
                                            fw="1.9.9"),
                                       log=messages.append)
    assert b"macros:" not in source.blob
    assert b"workbookVersionId" in source.blob  # always stamped
    assert any("WITHOUT the macro list" in m for m in messages)

    messages = []
    source = procedure.schedule_source(_ctx(programming=_programming(),
                                            fw="2.1.0"),
                                       log=messages.append)
    assert b"macros:" in source.blob
    assert any("2.1.0" in m and "macro(s)" in m for m in messages)


# ── the serial install path ──────────────────────────────────────────────────
class _WorkbookConsole:
    def __init__(self, workbook_reply=VERSION_ID, begin_reply="schedule begin: ready"):
        self.begin_reply = begin_reply
        self.workbook_reply = workbook_reply
        self.staged = b""
        self.committed = None
        self.status_calls = 0
        self.url_installed = None

    def command(self, cmd, timeout=5.0):
        if cmd == "schedule begin":
            self.staged = b""
            return f"schedule begin\n{self.begin_reply}\nambyte> "
        if cmd.startswith("schedule put "):
            self.staged += base64.b64decode(cmd[len("schedule put "):])
            return f"{cmd}\nschedule put: {len(self.staged)} bytes\nambyte> "
        if cmd.startswith("schedule commit "):
            self.committed = cmd.split()[2:]
            return f"{cmd}\nschedule commit queued: id=x\nambyte> "
        if cmd == "schedule abort":
            return "schedule abort: discarded\nambyte> "
        raise AssertionError(f"unexpected command {cmd!r}")

    def schedule_release(self, timeout=10.0):
        if self.committed is None:
            return ScheduleReleaseStatus("b" * 64, "0.9.0", "v1.8.1", "v1.8.1",
                                         True, True)
        sha, campaign, version, built = self.committed
        return ScheduleReleaseStatus(sha, version, built, built, True, True)

    def schedule_status_workbook(self, timeout=10.0):
        self.status_calls += 1
        return self.workbook_reply

    def schedule_install(self, *args):
        self.url_installed = args

    def wifi_connected(self, timeout=10.0):
        return True

    def close(self):
        pass


def _install(monkeypatch, console, ctx):
    console.schedule_push = AmbyteConsole.schedule_push.__get__(console)
    console._schedule_abort_quietly = (
        AmbyteConsole._schedule_abort_quietly.__get__(console))
    monkeypatch.setattr(ambyte_serial, "connect_after_boot",
                        lambda *a, **k: console)
    monkeypatch.setattr(procedure.time, "sleep", lambda _s: None)
    monkeypatch.setattr(procedure.release_fetch, "script_bytes",
                        lambda script, log=print: b"catalog bytes\n")
    return procedure.install_schedule_script(ctx, SimpleNamespace(port="COM7"),
                                             log=lambda _m: None)


def test_workbook_install_pushes_stamped_bytes_and_verifies_the_id(monkeypatch):
    console = _WorkbookConsole()
    result = _install(monkeypatch, console, _ctx(programming=_programming()))

    stamped = _stamped().encode("utf-8")
    assert console.staged == stamped
    sha, campaign, version, built = console.committed
    assert sha == hashlib.sha256(stamped).hexdigest()
    assert campaign == f"workbook-{VERSION_ID}"
    assert version == "wb-v3"
    assert built == "2.1.0"
    assert console.url_installed is None
    assert console.status_calls == 1
    assert result.passed
    assert f"workbook={VERSION_ID} confirmed" in result.detail


def test_workbook_install_fails_when_the_reported_id_differs(monkeypatch):
    console = _WorkbookConsole(workbook_reply="dddddddd-0000-0000-0000-000000000000")
    result = _install(monkeypatch, console, _ctx(programming=_programming()))
    assert not result.passed
    assert result.label == "Schedule workbook"
    assert VERSION_ID in result.detail


def test_workbook_install_tolerates_firmware_without_the_header_print(monkeypatch):
    console = _WorkbookConsole(workbook_reply=None)
    # schedule_status_workbook returns None on firmware that predates the
    # workbook= print; the sha identity already matched, so this passes.
    result = _install(monkeypatch, console, _ctx(programming=_programming()))
    assert result.passed


def test_workbook_install_has_no_url_fallback_for_old_firmware(monkeypatch):
    console = _WorkbookConsole(
        begin_reply="Usage: schedule <start|stop|status|release|install|exec>")
    with pytest.raises(ProcedureError, match="no release URL"):
        _install(monkeypatch, console, _ctx(programming=_programming()))
    assert console.url_installed is None


def test_catalog_install_is_unchanged(monkeypatch):
    console = _WorkbookConsole()
    result = _install(monkeypatch, console, _ctx())
    sha, campaign, version, built = console.committed
    assert (sha, campaign, version, built) == (
        CATALOG.sha256, CATALOG.campaign_id, CATALOG.script_version,
        CATALOG.built_against_fw)
    assert console.status_calls == 0  # no workbook check for a catalog asset
    assert result.passed
    assert "workbook" not in result.detail


# ── step 3: NVS provenance + littlefs bake ───────────────────────────────────
def test_prepare_provisioning_bakes_workbook_provenance(monkeypatch, tmp_path):
    from flash_gui import procedure as proc
    from flash_gui.config import ENVIRONMENTS
    from flash_gui.procedure import DeviceRun, PreflightInfo, SessionContext

    mac = "28:37:2F:FF:E7:04"
    thing = f"ambyte_{mac}"
    identity = SimpleNamespace(device_id="device-id", thing_name=thing,
                               certificate_pem="cert", private_key_pem="key",
                               rotated=False)
    onboarding = SimpleNamespace(
        thing_name=thing, device_type="ambyte",
        endpoint="server-owned.iot.example",
        topic_prefix="experiment/data_ingest/v1/experiment-id/ambyte")
    client = SimpleNamespace(
        provision_device=lambda serial, name, log=print: identity,
        onboard_device=lambda device_id, experiment_id: onboarding)
    ctx = SessionContext(
        env=ENVIRONMENTS["dev"], client=client,
        release=SimpleNamespace(version="2.1.0"), schedule_script=CATALOG,
        experiment_id="experiment-id", timezone="Europe/Amsterdam",
        wifi_ssid="bench-ap", wifi_password="secret",
        programming=_programming())
    run = DeviceRun(port="COM7", preflight=PreflightInfo("COM7", mac, False,
                                                         None),
                    name="Roof-3")

    captured = {}

    def fake_nvs(plan, path):
        captured["plan"] = plan
        return path

    def fake_image(blob, path):
        captured["blob"] = blob
        return path

    monkeypatch.setattr(proc, "build_nvs_image", fake_nvs)
    monkeypatch.setattr(proc, "build_schedule_image", fake_image)
    monkeypatch.setattr(proc, "CACHE_DIR", tmp_path)

    proc.prepare_provisioning(ctx, run, log=lambda _m: None)

    stamped = _stamped().encode("utf-8")
    plan = captured["plan"]
    assert captured["blob"] == stamped
    assert plan.schedule_sha256 == hashlib.sha256(stamped).hexdigest()
    assert plan.schedule_script_version == "wb-v3"
    assert plan.schedule_campaign_id == f"workbook-{VERSION_ID}"
    assert plan.schedule_built_against_fw == "2.1.0"

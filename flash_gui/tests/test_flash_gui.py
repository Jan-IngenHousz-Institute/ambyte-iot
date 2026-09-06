# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Unit tests for the pure logic in flash_gui (no hardware, no network).

Run from the repo root:  python -m pytest flash_gui/tests -q
"""

from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from flash_gui import config, gui, procedure                   # noqa: E402
from flash_gui import openjii_client, release_fetch, tls       # noqa: E402
from flash_gui.ambyte_serial import (AmbyteConsole, ConsoleError,  # noqa: E402
                                    ScheduleReleaseStatus, expand_mac_token)
from flash_gui.nvs_builder import (AMAZON_ROOT_CA1,            # noqa: E402
                                   NvsBuildError, ProvisioningPlan,
                                   build_nvs_csv, build_nvs_image)
from flash_gui.openjii_client import (DeviceIdentity, DeviceOnboarding,  # noqa: E402
                                     OpenJIIError)
from flash_gui.procedure import (DeviceRun, PreflightInfo, SessionContext,  # noqa: E402
                                 clean_device_name, mqtt_uri_from_endpoint)
from flash_gui.release_fetch import (ReleaseError, ReleaseImages,  # noqa: E402
                                     ScheduleScriptRelease,
                                     fetch_latest_schedule_catalog,
                                     pick_firmware_release,
                                     pick_schedule_release)


MAC = "E8:F6:0A:B1:1F:34"
THING = "ambyte_E8:F6:0A:B1:1F:34"


def test_gui_doubles_display_and_replaces_unscalable_bitmap_fonts(monkeypatch):
    calls = []
    monkeypatch.setattr(gui.sys, "platform", "linux")

    class FakeFont:
        def __init__(self, size, linespace):
            self.size = size
            self.linespace = linespace
            self.family = "fixed"

        def actual(self, key):
            assert key == "family"
            return self.family

        def cget(self, key):
            assert key == "size"
            return self.size

        def metrics(self, key):
            assert key == "linespace"
            return self.linespace

        def configure(self, *, family, size):
            self.family = family
            self.size = size

    class FakeTk:
        def call(self, *args):
            calls.append(args)
            return 1.6 if len(args) == 2 else None

    fonts = {
        "TkDefaultFont": FakeFont(10, 13),
        "TkFixedFont": FakeFont(-12, 15),
    }
    root = SimpleNamespace(tk=FakeTk())
    monkeypatch.setattr(gui.tkfont, "names", lambda *, root: fonts)
    monkeypatch.setattr(gui.tkfont, "nametofont",
                        lambda name, *, root: fonts[name])
    monkeypatch.setattr(gui.tkfont, "families",
                        lambda *, root: ("liberation sans", "liberation mono"))

    gui.apply_ui_scale(root)

    assert calls == [("tk", "scaling"), ("tk", "scaling", 3.2)]
    assert (fonts["TkDefaultFont"].family,
            fonts["TkDefaultFont"].size) == ("liberation sans", -26)
    assert (fonts["TkFixedFont"].family,
            fonts["TkFixedFont"].size) == ("liberation mono", -30)


@pytest.mark.parametrize(("platform", "family"), [
    ("win32", "Segoe UI"),
    ("darwin", "Helvetica"),
    ("linux", "Noto Sans"),
])
def test_gui_preserves_native_scalable_platform_dpi_and_fonts(
        monkeypatch, platform, family):
    calls = []

    class FakeFont:
        configured = False

        def actual(self, key):
            assert key == "family"
            return family

        def metrics(self, key):
            assert key == "linespace"
            return 13

        def configure(self, **_options):
            self.configured = True

    class FakeTk:
        def call(self, *args):
            calls.append(args)
            return 1.5

    font = FakeFont()
    root = SimpleNamespace(tk=FakeTk())
    monkeypatch.setattr(gui.sys, "platform", platform)
    monkeypatch.setattr(gui.tkfont, "names",
                        lambda *, root: ("TkDefaultFont",))
    monkeypatch.setattr(gui.tkfont, "nametofont",
                        lambda name, *, root: font)
    monkeypatch.setattr(gui.tkfont, "families",
                        lambda *, root: ("segoe ui",))

    gui.apply_ui_scale(root)

    assert calls == []
    assert font.configured is False


# ── frozen-app TLS trust ────────────────────────────────────────────────────
def test_tls_context_adds_bundled_ca_to_platform_roots(monkeypatch):
    class FakeContext:
        def __init__(self):
            self.loaded_cafile = None

        def load_verify_locations(self, *, cafile):
            self.loaded_cafile = cafile

    context = FakeContext()
    tls.ssl_context.cache_clear()
    monkeypatch.setattr(tls.ssl, "create_default_context", lambda: context)
    monkeypatch.setattr(tls.certifi, "where", lambda: "/bundle/cacert.pem")

    assert tls.ssl_context() is context
    assert context.loaded_cafile == "/bundle/cacert.pem"
    tls.ssl_context.cache_clear()


@pytest.mark.parametrize("module", [openjii_client, release_fetch])
def test_https_clients_pass_portable_tls_context(monkeypatch, module):
    sentinel = object()
    seen = {}

    class Response:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self):
            return b'{}'

    def fake_urlopen(_request, **kwargs):
        seen.update(kwargs)
        return Response()

    monkeypatch.setattr(module, "ssl_context", lambda: sentinel)
    monkeypatch.setattr(module.urllib.request, "urlopen", fake_urlopen)

    if module is openjii_client:
        client = openjii_client.OpenJIIClient(config.ENVIRONMENTS["dev"],
                                              "jii_test")
        client._request("GET", "/health")
    else:
        release_fetch._get_json("https://api.github.com/test")

    assert seen["context"] is sentinel


def make_plan(**over) -> ProvisioningPlan:
    base = dict(
        device_name="Roof-3",
        timezone="Europe/Amsterdam",
        mqtt_uri="mqtts://example-ats.iot.eu-central-1.amazonaws.com:8883",
        client_id=THING,
        topic_root=f"experiment/data_ingest/v1/abc/multispeq/v1.0/{THING}",
        command_topic=f"device/scripts/v1/Ambyte/2/{THING}",
        status_topic=f"experiment/data_ingest/v1/abc/multispeq/v1.0/{THING}/status",
        device_cert_pem="-----BEGIN CERTIFICATE-----\nAAA\n-----END CERTIFICATE-----\n",
        device_key_pem="-----BEGIN RSA PRIVATE KEY-----\nBBB\n-----END RSA PRIVATE KEY-----\n",
        wifi_ssid="Fionas Garden",
        wifi_password="secret",
        device_id="03:25:07:04",
        protocol_id="3517",
        device_version="1",
        device_firmware="1",
        firmware_version="1.6.0",
    )
    base.update(over)
    return ProvisioningPlan(**base)


# ── name validation ──────────────────────────────────────────────────────────
@pytest.mark.parametrize("name,expected", [
    ("Roof-3", "Roof-3"),
    ("  padded  ", "padded"),
    ("name with spaces", "name with spaces"),
    ("", None),
    ("   ", None),
    ('with"quote', None),
    ("with\\backslash", None),
    ("tab\tchar", None),
    ("ünïcode", None),
    ("x" * 63, "x" * 63),
    ("x" * 64, None),
])
def test_clean_device_name(name, expected):
    assert clean_device_name(name) == expected


# ── proposed-name logic ──────────────────────────────────────────────────────
def test_proposed_name_falls_back_to_mac():
    info = PreflightInfo(port="COM7", mac=MAC, had_console=False,
                         stored_name=None)
    assert info.proposed_name == f"AMBYTE_{MAC}"


def test_proposed_name_prefers_stored():
    info = PreflightInfo(port="COM7", mac=MAC, had_console=True,
                         stored_name="Roof-3")
    assert info.proposed_name == "Roof-3"


def test_mac_token_expansion():
    assert expand_mac_token("AMBYTE_{MAC}", MAC) == f"AMBYTE_{MAC}"
    assert expand_mac_token("Roof-3", MAC) == "Roof-3"


# ── openJII onboarding contract ─────────────────────────────────────────────
def test_onboard_device_posts_binding_and_uses_selected_server_config(monkeypatch):
    client = openjii_client.OpenJIIClient(config.ENVIRONMENTS["dev"], "jii_test")
    calls = []
    payload = {
        "thingName": THING,
        "deviceType": "ambyte",
        "endpoint": "example-ats.iot.eu-central-1.amazonaws.com",
        "experiments": [
            {"experimentId": "other", "topicPrefix": "wrong/prefix"},
            {"experimentId": "selected", "topicPrefix": "/server/topic/prefix/"},
        ],
    }

    def request(method, path, body=None, timeout=60):
        calls.append((method, path, body, timeout))
        return 200, payload

    monkeypatch.setattr(client, "_request", request)

    result = client.onboard_device("device-id", "selected")

    assert result == DeviceOnboarding(
        thing_name=THING,
        device_type="ambyte",
        endpoint="example-ats.iot.eu-central-1.amazonaws.com",
        topic_prefix="server/topic/prefix",
    )
    assert calls == [(
        "POST",
        "/api/v1/devices/device-id/onboard",
        {"experimentIds": ["selected"], "includeWorkbook": False},
        60,
    )]


def test_onboard_device_rejects_config_without_selected_topic(monkeypatch):
    client = openjii_client.OpenJIIClient(config.ENVIRONMENTS["dev"], "jii_test")
    monkeypatch.setattr(client, "_request", lambda *_args, **_kwargs: (200, {
        "thingName": THING,
        "deviceType": "ambyte",
        "endpoint": "example.test",
        "experiments": [],
    }))

    with pytest.raises(OpenJIIError, match="topicPrefix for experiment selected"):
        client.onboard_device("device-id", "selected")


@pytest.mark.parametrize("endpoint,expected", [
    ("example-ats.iot.eu-central-1.amazonaws.com",
     "mqtts://example-ats.iot.eu-central-1.amazonaws.com:8883"),
    ("mqtts://example.test", "mqtts://example.test:8883"),
    ("mqtts://example.test:443/", "mqtts://example.test:443"),
])
def test_mqtt_uri_comes_from_onboarding_endpoint(endpoint, expected):
    assert mqtt_uri_from_endpoint(endpoint) == expected


@pytest.mark.parametrize("endpoint", ["", "https://example.test", "mqtts://user@example.test"])
def test_mqtt_uri_rejects_unsupported_endpoint(endpoint):
    with pytest.raises(ValueError):
        mqtt_uri_from_endpoint(endpoint)


def test_prepare_provisioning_uses_openjii_endpoint_topic_and_binding(monkeypatch,
                                                                    tmp_path):
    identity = DeviceIdentity(
        device_id="device-id",
        thing_name=THING,
        certificate_pem="certificate",
        private_key_pem="private-key",
        bundle_dir=tmp_path,
        rotated=False,
    )
    onboarding = DeviceOnboarding(
        thing_name=THING,
        device_type="ambyte",
        endpoint="server-owned.iot.example",
        topic_prefix="experiment/data_ingest/v1/experiment-id/ambyte",
    )

    class Client:
        def __init__(self):
            self.calls = []

        def provision_device(self, serial, name, log=print):
            self.calls.append(("provision", serial, name))
            return identity

        def onboard_device(self, device_id, experiment_id):
            self.calls.append(("onboard", device_id, experiment_id))
            return onboarding

    client = Client()
    context = SessionContext(
        env=config.ENVIRONMENTS["dev"],
        client=client,
        release=SimpleNamespace(version="1.6.0"),
        schedule_script=SimpleNamespace(
            asset_name="default.yaml",
            sha256="abc123",
            script_version="2.0.0",
            built_against_fw="1.6.0",
            campaign_id="schedule-v2.0.0",
        ),
        experiment_id="experiment-id",
        timezone="Europe/Amsterdam",
        wifi_ssid="Fionas Garden",
        wifi_password="secret",
    )
    run = DeviceRun(
        port="COM7",
        preflight=PreflightInfo("COM7", MAC, False, None),
        name="Roof-3",
    )
    monkeypatch.setattr(procedure, "build_nvs_image",
                        lambda plan, path: path)
    monkeypatch.setattr(procedure.release_fetch, "script_bytes",
                        lambda script, log=None: b"print('ready')\n")
    monkeypatch.setattr(procedure, "build_schedule_image",
                        lambda blob, path: path)

    procedure.prepare_provisioning(context, run, log=lambda _message: None)

    assert client.calls == [
        ("provision", MAC, "Roof-3"),
        ("onboard", "device-id", "experiment-id"),
    ]
    assert run.plan.mqtt_uri == "mqtts://server-owned.iot.example:8883"
    assert run.plan.topic_root == (
        "experiment/data_ingest/v1/experiment-id/ambyte/v1.0/" + THING)
    assert run.plan.client_id == THING


def test_status_topic_is_root_plus_status():
    assert config.default_status_topic("a/b/c/") == "a/b/c/status"


def test_command_topic_carries_thing_name():
    assert config.default_command_topic(THING).endswith("/" + THING)


# ── NVS CSV generation ───────────────────────────────────────────────────────
def test_nvs_csv_structure():
    csv = build_nvs_csv(make_plan(), flash_time=1786000000)
    lines = csv.splitlines()
    assert lines[0] == "key,type,encoding,value"
    # namespaces appear exactly once, in order
    ns = [ln.split(",")[0] for ln in lines if ",namespace," in ln]
    assert ns == ["device_cfg", "certs", "wifi_creds", "wifi_prov"]
    assert "flash_time,data,u32,1786000000" in csv
    assert "provisioned,data,u8,1" in csv
    # PEMs are CSV-quoted (contain newlines)
    assert '"-----BEGIN CERTIFICATE-----' in csv
    # NVS key names differ from the console names for these:
    assert "cmd_topic,data,string," in csv
    assert "device_ver,data,string," in csv
    assert "device_firm,data,string," in csv
    assert "firmware_ver,data,string," in csv


def test_nvs_csv_optional_site_metadata():
    csv = build_nvs_csv(
        make_plan(lat=52.173, lon=5.819, deployment="greenhouse-a"),
        flash_time=1786000000,
    )
    from tools.site_state_blob import encode_site_state

    expected = encode_site_state(52.173, 5.819, "greenhouse-a").hex()
    assert f"site_state,data,hex2bin,{expected}" in csv
    assert "lat,data," not in csv
    assert "lon,data," not in csv
    assert "deployment,data," not in csv


def test_nvs_csv_rejects_oversized_deployment():
    with pytest.raises(NvsBuildError, match="63-byte"):
        build_nvs_csv(make_plan(deployment="x" * 64))


@pytest.mark.parametrize(("field", "value"), [
    ("lat", float("nan")),
    ("lat", 90.01),
    ("lon", float("inf")),
    ("lon", -180.01),
])
def test_nvs_csv_rejects_invalid_site_coordinates(field, value):
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(**{field: value}))


def test_nvs_csv_rejects_missing_required():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(wifi_ssid=""))


def test_nvs_csv_rejects_oversized_cert():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(device_cert_pem="x" * 3000))


def test_nvs_csv_rejects_long_timezone():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(timezone="A/" + "b" * 50))


# ── Schedule provenance in NVS + the littlefs schedule.yaml image ────────────────────
SCHEDULE_SHA = "ab" * 32


def test_nvs_csv_schedule_provenance_absent_by_default():
    csv = build_nvs_csv(make_plan(), flash_time=1786000000)
    assert "script_upd" not in csv


def test_nvs_csv_schedule_provenance_rows():
    csv = build_nvs_csv(make_plan(schedule_sha256=SCHEDULE_SHA,
                                  schedule_script_version="1.2.0",
                                  schedule_built_against_fw="1.7.0",
                                  schedule_campaign_id="schedule-v1.2.0"),
                        flash_time=1786000000)
    ns = [ln.split(",")[0] for ln in csv.splitlines() if ",namespace," in ln]
    assert ns[-1] == "script_upd"
    assert f'script_sha,data,string,"{SCHEDULE_SHA}"' in csv
    assert 'applied_id,data,string,"schedule-v1.2.0"' in csv
    assert 'script_ver,data,string,"1.2.0"' in csv
    assert 'built_fw,data,string,"1.7.0"' in csv
    # install_fw is the firmware release being flashed
    assert 'install_fw,data,string,"1.6.0"' in csv


def test_nvs_csv_schedule_provenance_all_or_none():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(schedule_sha256=SCHEDULE_SHA))


def test_nvs_csv_schedule_provenance_bad_sha():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(schedule_sha256="zz",
                                schedule_script_version="1.2.0",
                                schedule_built_against_fw="1.7.0",
                                schedule_campaign_id="schedule-v1.2.0"))


def test_littlefs_image_roundtrip(tmp_path):
    from flash_gui.littlefs_image import build_schedule_image
    script = b'schema: jii.ambyte-schedule/v1-draft\nactions: []\n'
    out = build_schedule_image(script, tmp_path / "littlefs.bin")
    assert out.is_file()
    assert out.stat().st_size == config.LITTLEFS_PARTITION_SIZE
    # independent read-back via a fresh mount of the written bytes
    from littlefs import LittleFS, UserContext
    ctx = UserContext(buffer=bytearray(out.read_bytes()))
    fs = LittleFS(context=ctx, mount=True, block_size=4096,
                  block_count=config.LITTLEFS_PARTITION_SIZE // 4096,
                  read_size=128, prog_size=128, cache_size=512,
                  lookahead_size=128, block_cycles=512, name_max=64)
    with fs.open("schedule.yaml", "rb") as f:
        assert f.read() == script
    fs.unmount()


def test_littlefs_image_rejects_empty(tmp_path):
    from flash_gui.littlefs_image import (LittlefsImageError,
                                          build_schedule_image)
    with pytest.raises(LittlefsImageError):
        build_schedule_image(b"", tmp_path / "littlefs.bin")


def test_littlefs_image_rejects_schedule_larger_than_firmware_limit(tmp_path):
    from flash_gui.littlefs_image import (LittlefsImageError,
                                          build_schedule_image)
    with pytest.raises(LittlefsImageError, match="16384-byte limit"):
        build_schedule_image(b"x" * (16 * 1024 + 1), tmp_path / "littlefs.bin")


def test_packaged_littlefs_smoke_seam(tmp_path):
    from flash_gui.packaged_smoke import run_littlefs_smoke

    report = tmp_path / "result.txt"
    run_littlefs_smoke(report)
    assert report.read_text().startswith("PASS:")


def test_nvs_image_generation(tmp_path):
    out = build_nvs_image(make_plan(ca_cert_pem=AMAZON_ROOT_CA1),
                          tmp_path / "nvs.bin", flash_time=1786000000)
    assert out.is_file()
    assert out.stat().st_size == config.NVS_PARTITION_SIZE


# ── release manifest parsing ─────────────────────────────────────────────────
def test_release_flash_files_sorted_and_nvs_free(tmp_path):
    manifest = {
        "flash_files": {
            "0x20000": "ambyte-iot.bin",
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
        },
        "flash_settings": {"flash_mode": "dio", "flash_size": "16MB",
                           "flash_freq": "80m"},
        "extra_esptool_args": {"chip": "esp32s3"},
    }
    for rel in manifest["flash_files"].values():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"\x00")
    rel_images = ReleaseImages("v1.6.0", "1.6.0", tmp_path, manifest)
    offsets = [off for off, _ in rel_images.flash_files]
    assert offsets == sorted(offsets)
    assert config.NVS_OFFSET not in offsets   # nvs is spliced in by the caller
    assert rel_images.flash_settings["flash_size"] == "16MB"
    assert rel_images.chip == "esp32s3"


def test_release_missing_file_raises(tmp_path):
    manifest = {"flash_files": {"0x0": "missing.bin"}}
    with pytest.raises(ReleaseError):
        _ = ReleaseImages("v1.6.0", "1.6.0", tmp_path, manifest).flash_files


# ── firmware release picker ──────────────────────────────────────────────────
def _rel(tag, assets=("ambyte-iot-" + "v1.0.0" + ".zip",), published="2026-01-01T00:00:00Z",
         prerelease=False, draft=False):
    return {"tag_name": tag, "prerelease": prerelease, "draft": draft,
            "published_at": published,
            "assets": [{"name": a} for a in assets]}


def test_picker_skips_flash_gui_and_schedule_releases():
    # The exact incident: flash-gui-v0.1.0 published AFTER v1.6.1 hijacked
    # /releases/latest and carried no firmware zip.
    releases = [
        _rel("flash-gui-v0.1.0", assets=("ambyte-flash-gui-windows.zip",),
             published="2026-08-07T07:27:43Z"),
        _rel("schedule-v1.2.0", assets=("schedule-release.zip",),
             published="2026-08-07T00:00:00Z"),
        _rel("v1.6.1", assets=("firmware.bin", "ambyte-iot-v1.6.1.zip"),
             published="2026-08-06T22:03:47Z"),
        _rel("v1.6.0", assets=("firmware.bin", "ambyte-iot-v1.6.0.zip"),
             published="2026-08-05T19:40:47Z"),
    ]
    assert pick_firmware_release(releases)["tag_name"] == "v1.6.1"


def test_picker_skips_prerelease_draft_and_assetless():
    releases = [
        _rel("v2.0.0", prerelease=True, published="2026-09-01T00:00:00Z"),
        _rel("v1.9.0", draft=True, published="2026-08-30T00:00:00Z"),
        _rel("v1.8.0", assets=("firmware.bin",),   # zip missing
             published="2026-08-20T00:00:00Z"),
        _rel("v1.7.0", assets=("ambyte-iot-v1.7.0.zip",),
             published="2026-08-10T00:00:00Z"),
    ]
    assert pick_firmware_release(releases)["tag_name"] == "v1.7.0"


def test_picker_returns_none_when_no_firmware_release():
    releases = [
        _rel("flash-gui-v0.1.0", assets=("ambyte-flash-gui-linux.tar.gz",)),
        _rel("schedule-v1.0.0", assets=("schedule.zip",)),
    ]
    assert pick_firmware_release(releases) is None
    assert pick_firmware_release([]) is None


# ── Schedule release catalog picker ───────────────────────────────────────────────
def _schedule_rel(tag, scripts=("default",), published="2026-01-01T00:00:00Z",
             prerelease=False, draft=False):
    assets = []
    for script in scripts:
        assets.extend((
            {"name": f"{script}.yaml", "size": 10,
             "browser_download_url": f"https://example.test/{tag}/{script}.yaml"},
            {"name": f"{script}.yaml.manifest.json", "size": 500,
             "browser_download_url":
                 f"https://example.test/{tag}/{script}.yaml.manifest.json"},
        ))
    return {"tag_name": tag, "prerelease": prerelease, "draft": draft,
            "published_at": published, "assets": assets}


def _schedule_manifest(tag="schedule-v1.2.3", script="default"):
    version = tag.removeprefix("schedule-v")
    asset_url = f"https://example.test/{tag}/{script}.yaml"
    digest = "a" * 64
    campaign = tag if script == "default" else f"{tag}:{script}"
    return {
        "schema_version": 1,
        "script_name": script,
        "script_version": version,
        "tag": tag,
        "sha256": digest,
        "size_bytes": 10,
        "built_against_fw": "1.7.0",
        "asset_url": asset_url,
        "script_update": {
            "type": "script_update",
            "id": campaign,
            "url": asset_url,
            "checksum": digest,
            "script_version": version,
            "built_against_fw": "1.7.0",
        },
    }


def test_schedule_picker_uses_highest_stable_catalog_version():
    releases = [
        _schedule_rel("schedule-v1.9.0", published="2026-09-01T00:00:00Z"),
        _schedule_rel("schedule-v2.0.0", published="2026-08-01T00:00:00Z"),
        _schedule_rel("schedule-v3.0.0", prerelease=True),
        _schedule_rel("v9.0.0"),
        _schedule_rel("schedule-v2.1.0", scripts=()),
    ]
    assert pick_schedule_release(releases)["tag_name"] == "schedule-v2.0.0"


def test_fetch_latest_schedule_catalog_validates_and_lists_all_scripts(monkeypatch):
    release = _schedule_rel("schedule-v1.2.3", scripts=("default", "legacy_1hz_spec"))
    responses = {
        "https://example.test/schedule-v1.2.3/default.yaml.manifest.json":
            _schedule_manifest(script="default"),
        "https://example.test/schedule-v1.2.3/legacy_1hz_spec.yaml.manifest.json":
            _schedule_manifest(script="legacy_1hz_spec"),
    }

    monkeypatch.setattr(release_fetch, "_release_list", lambda log=None: [release])
    monkeypatch.setattr(release_fetch, "_get_json", lambda url: responses[url])
    catalog = fetch_latest_schedule_catalog(log=lambda _message: None)
    assert catalog.tag == "schedule-v1.2.3"
    assert [script.asset_name for script in catalog.scripts] == [
        "default.yaml", "legacy_1hz_spec.yaml"]
    assert catalog.scripts[1].campaign_id == "schedule-v1.2.3:legacy_1hz_spec"


def test_fetch_latest_schedule_catalog_rejects_manifest_drift(monkeypatch):
    release = _schedule_rel("schedule-v1.2.3")
    manifest = _schedule_manifest()
    manifest["sha256"] = "not-a-digest"
    monkeypatch.setattr(release_fetch, "_release_list", lambda log=None: [release])
    monkeypatch.setattr(release_fetch, "_get_json", lambda url: manifest)
    with pytest.raises(ReleaseError, match="sha256"):
        fetch_latest_schedule_catalog(log=lambda _message: None)


def test_console_parses_schedule_release_and_queues_immutable_install():
    console = object.__new__(AmbyteConsole)
    commands = []

    def command(cmd, timeout=5.0):
        commands.append((cmd, timeout))
        if cmd == "schedule release":
            return ("schedule release: sha256=" + "a" * 64
                    + " version=1.2.3 built_against_fw=1.7.0 "
                    "installed_on_fw=1.7.1 verified=true running=true\n"
                    "ambyte> ")
        return "schedule install queued: id=schedule-v1.2.3\nambyte> "

    console.command = command
    status = console.schedule_release()
    assert status.sha256 == "a" * 64
    assert status.verified and status.running
    console.schedule_install(
        "https://example.test/schedule-v1.2.3/default.yaml", "a" * 64,
        "schedule-v1.2.3", "1.2.3", "1.7.0")
    assert commands[-1][0].startswith("schedule install https://example.test/")


def test_connect_after_boot_keeps_port_open_while_waiting(monkeypatch):
    """Polling must not reset the ESP32 by reopening its USB console."""
    created = []

    class SlowConsole:
        def __init__(self, port):
            self.port = port
            self.polls = 0
            self.closed = False
            created.append(self)

        def wait_prompt(self, timeout):
            self.polls += 1
            return self.polls == 3

        def close(self):
            self.closed = True

    monkeypatch.setattr(procedure.ambyte_serial, "AmbyteConsole", SlowConsole)
    monkeypatch.setattr(
        procedure.ambyte_serial, "esp_jtag_ports", lambda: ["/dev/ttyACM0"])

    console = procedure.ambyte_serial.connect_after_boot(
        "/dev/ttyACM0", deadline_s=30.0)

    assert console is created[0]
    assert len(created) == 1
    assert console.polls == 3
    assert not console.closed


def test_onboarding_installs_selected_script_when_sd_has_no_identity(monkeypatch):
    script = ScheduleScriptRelease(
        tag="schedule-v1.2.3",
        asset_name="default.yaml",
        script_name="default",
        script_version="1.2.3",
        built_against_fw="1.7.0",
        asset_url="https://example.test/schedule-v1.2.3/default.yaml",
        sha256="a" * 64,
        size_bytes=10,
        campaign_id="schedule-v1.2.3",
    )

    class FakeConsole:
        def __init__(self):
            self.status_reads = 0
            self.install_args = None
            self.close_count = 0

        def schedule_release(self, timeout=10.0):
            self.status_reads += 1
            if self.status_reads == 1:
                raise ConsoleError("default.yaml is absent")
            if self.status_reads == 2:
                raise ConsoleError("'schedule release' got no prompt back within 20s")
            return ScheduleReleaseStatus(
                sha256="a" * 64,
                script_version="1.2.3",
                built_against_fw="1.7.0",
                installed_on_fw="1.7.1",
                verified=True,
                running=True,
            )

        def schedule_install(self, *args):
            self.install_args = args

        def wifi_connected(self, timeout=10.0):
            return True

        def close(self):
            self.close_count += 1

    console = FakeConsole()
    connects = []

    def connect(*_args, **_kwargs):
        connects.append(True)
        return console

    monkeypatch.setattr(
        procedure.ambyte_serial, "connect_after_boot",
        connect)
    monkeypatch.setattr(procedure.time, "sleep", lambda _seconds: None)

    result = procedure.install_schedule_script(
        SimpleNamespace(schedule_script=script, wifi_ssid="test-ap"),
        SimpleNamespace(port="/dev/ttyACM0"),
        log=lambda _message: None,
    )
    assert result.passed
    assert len(connects) == 1
    assert console.close_count == 1  # the normal function-finally close only
    assert console.install_args == (
        script.asset_url, script.sha256, script.campaign_id,
        script.script_version, script.built_against_fw)


# ── timezone helpers ─────────────────────────────────────────────────────────
def test_firmware_zone_table():
    from flash_gui.timezones import FIRMWARE_SUPPORTED_ZONES, firmware_supports
    assert firmware_supports("Europe/Amsterdam")
    assert firmware_supports("UTC")
    # The whole IANA database is compiled into the firmware since 1.10.1 — these
    # are the on-boardings that used to fail verification unrepairably.
    assert firmware_supports("America/La_Paz")
    assert firmware_supports("America/Manaus")
    assert firmware_supports("Asia/Calcutta")        # deprecated aliases too
    assert not firmware_supports("Mars/Olympus")
    assert not firmware_supports("")
    assert len(FIRMWARE_SUPPORTED_ZONES) > 400

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

from flash_gui import config, procedure                        # noqa: E402
from flash_gui import openjii_client, release_fetch, tls       # noqa: E402
from flash_gui.ambyte_serial import (AmbyteConsole, ConsoleError,  # noqa: E402
                                    LuaReleaseStatus, expand_mac_token)
from flash_gui.nvs_builder import (AMAZON_ROOT_CA1,            # noqa: E402
                                   NvsBuildError, ProvisioningPlan,
                                   build_nvs_csv, build_nvs_image)
from flash_gui.procedure import PreflightInfo, clean_device_name  # noqa: E402
from flash_gui.release_fetch import (ReleaseError, ReleaseImages,  # noqa: E402
                                     LuaScriptRelease,
                                     fetch_latest_lua_catalog,
                                     pick_firmware_release,
                                     pick_lua_release)


MAC = "E8:F6:0A:B1:1F:34"
THING = "ambyte_E8:F6:0A:B1:1F:34"


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


# ── topic derivation ─────────────────────────────────────────────────────────
def test_default_topic_root():
    root = config.default_topic_root("011a2000-3b00-44bd-a3c0-bed319f44f26",
                                     THING)
    assert root == ("experiment/data_ingest/v1/"
                    "011a2000-3b00-44bd-a3c0-bed319f44f26/"
                    f"multispeq/v1.0/{THING}")


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


def test_nvs_csv_rejects_missing_required():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(wifi_ssid=""))


def test_nvs_csv_rejects_oversized_cert():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(device_cert_pem="x" * 3000))


def test_nvs_csv_rejects_long_timezone():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(timezone="A/" + "b" * 50))


# ── Lua provenance in NVS + the littlefs main.lua image ─────────────────────
LUA_SHA = "ab" * 32


def test_nvs_csv_lua_provenance_absent_by_default():
    csv = build_nvs_csv(make_plan(), flash_time=1786000000)
    assert "script_upd" not in csv


def test_nvs_csv_lua_provenance_rows():
    csv = build_nvs_csv(make_plan(lua_sha256=LUA_SHA,
                                  lua_script_version="1.2.0",
                                  lua_built_against_fw="1.7.0",
                                  lua_campaign_id="lua-v1.2.0"),
                        flash_time=1786000000)
    ns = [ln.split(",")[0] for ln in csv.splitlines() if ",namespace," in ln]
    assert ns[-1] == "script_upd"
    assert f'script_sha,data,string,"{LUA_SHA}"' in csv
    assert 'applied_id,data,string,"lua-v1.2.0"' in csv
    assert 'script_ver,data,string,"1.2.0"' in csv
    assert 'built_fw,data,string,"1.7.0"' in csv
    # install_fw is the firmware release being flashed
    assert 'install_fw,data,string,"1.6.0"' in csv


def test_nvs_csv_lua_provenance_all_or_none():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(lua_sha256=LUA_SHA))


def test_nvs_csv_lua_provenance_bad_sha():
    with pytest.raises(NvsBuildError):
        build_nvs_csv(make_plan(lua_sha256="zz",
                                lua_script_version="1.2.0",
                                lua_built_against_fw="1.7.0",
                                lua_campaign_id="lua-v1.2.0"))


def test_littlefs_image_roundtrip(tmp_path):
    from flash_gui.littlefs_image import build_main_lua_image
    script = b'print("hello from main.lua")\n' * 40
    out = build_main_lua_image(script, tmp_path / "littlefs.bin")
    assert out.is_file()
    assert out.stat().st_size == config.LITTLEFS_PARTITION_SIZE
    # independent read-back via a fresh mount of the written bytes
    from littlefs import LittleFS, UserContext
    ctx = UserContext(buffer=bytearray(out.read_bytes()))
    fs = LittleFS(context=ctx, mount=True, block_size=4096,
                  block_count=config.LITTLEFS_PARTITION_SIZE // 4096,
                  read_size=128, prog_size=128, cache_size=512,
                  lookahead_size=128, block_cycles=512, name_max=64)
    with fs.open("main.lua", "rb") as f:
        assert f.read() == script
    fs.unmount()


def test_littlefs_image_rejects_empty(tmp_path):
    from flash_gui.littlefs_image import (LittlefsImageError,
                                          build_main_lua_image)
    with pytest.raises(LittlefsImageError):
        build_main_lua_image(b"", tmp_path / "littlefs.bin")


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


def test_picker_skips_flash_gui_and_lua_releases():
    # The exact incident: flash-gui-v0.1.0 published AFTER v1.6.1 hijacked
    # /releases/latest and carried no firmware zip.
    releases = [
        _rel("flash-gui-v0.1.0", assets=("ambyte-flash-gui-windows.zip",),
             published="2026-08-07T07:27:43Z"),
        _rel("lua-v1.2.0", assets=("lua-release.zip",),
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
        _rel("lua-v1.0.0", assets=("lua.zip",)),
    ]
    assert pick_firmware_release(releases) is None
    assert pick_firmware_release([]) is None


# ── Lua release catalog picker ───────────────────────────────────────────────
def _lua_rel(tag, scripts=("main",), published="2026-01-01T00:00:00Z",
             prerelease=False, draft=False):
    assets = []
    for script in scripts:
        assets.extend((
            {"name": f"{script}.lua", "size": 10,
             "browser_download_url": f"https://example.test/{tag}/{script}.lua"},
            {"name": f"{script}.lua.manifest.json", "size": 500,
             "browser_download_url":
                 f"https://example.test/{tag}/{script}.lua.manifest.json"},
        ))
    return {"tag_name": tag, "prerelease": prerelease, "draft": draft,
            "published_at": published, "assets": assets}


def _lua_manifest(tag="lua-v1.2.3", script="main"):
    version = tag.removeprefix("lua-v")
    asset_url = f"https://example.test/{tag}/{script}.lua"
    digest = "a" * 64
    campaign = tag if script == "main" else f"{tag}:{script}"
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


def test_lua_picker_uses_highest_stable_catalog_version():
    releases = [
        _lua_rel("lua-v1.9.0", published="2026-09-01T00:00:00Z"),
        _lua_rel("lua-v2.0.0", published="2026-08-01T00:00:00Z"),
        _lua_rel("lua-v3.0.0", prerelease=True),
        _lua_rel("v9.0.0"),
        _lua_rel("lua-v2.1.0", scripts=()),
    ]
    assert pick_lua_release(releases)["tag_name"] == "lua-v2.0.0"


def test_fetch_latest_lua_catalog_validates_and_lists_all_scripts(monkeypatch):
    release = _lua_rel("lua-v1.2.3", scripts=("main", "legacy_1Hz_spec"))
    responses = {
        "https://example.test/lua-v1.2.3/main.lua.manifest.json":
            _lua_manifest(script="main"),
        "https://example.test/lua-v1.2.3/legacy_1Hz_spec.lua.manifest.json":
            _lua_manifest(script="legacy_1Hz_spec"),
    }

    monkeypatch.setattr(release_fetch, "_release_list", lambda log=None: [release])
    monkeypatch.setattr(release_fetch, "_get_json", lambda url: responses[url])
    catalog = fetch_latest_lua_catalog(log=lambda _message: None)
    assert catalog.tag == "lua-v1.2.3"
    assert [script.asset_name for script in catalog.scripts] == [
        "legacy_1Hz_spec.lua", "main.lua"]
    assert catalog.scripts[0].campaign_id == "lua-v1.2.3:legacy_1Hz_spec"


def test_fetch_latest_lua_catalog_rejects_manifest_drift(monkeypatch):
    release = _lua_rel("lua-v1.2.3")
    manifest = _lua_manifest()
    manifest["sha256"] = "not-a-digest"
    monkeypatch.setattr(release_fetch, "_release_list", lambda log=None: [release])
    monkeypatch.setattr(release_fetch, "_get_json", lambda url: manifest)
    with pytest.raises(ReleaseError, match="sha256"):
        fetch_latest_lua_catalog(log=lambda _message: None)


def test_console_parses_lua_release_and_queues_immutable_install():
    console = object.__new__(AmbyteConsole)
    commands = []

    def command(cmd, timeout=5.0):
        commands.append((cmd, timeout))
        if cmd == "lua release":
            return ("lua release: sha256=" + "a" * 64
                    + " version=1.2.3 built_against_fw=1.7.0 "
                    "installed_on_fw=1.7.1 verified=true running=true\n"
                    "ambyte> ")
        return "lua install queued: id=lua-v1.2.3\nambyte> "

    console.command = command
    status = console.lua_release()
    assert status.sha256 == "a" * 64
    assert status.verified and status.running
    console.lua_install(
        "https://example.test/lua-v1.2.3/main.lua", "a" * 64,
        "lua-v1.2.3", "1.2.3", "1.7.0")
    assert commands[-1][0].startswith("lua install https://example.test/")


def test_connect_after_boot_keeps_port_open_while_waiting(monkeypatch):
    """Polling must not reset the ESP32 by reopening its USB console."""
    created = []

    class SlowConsole:
        rx_total = 0

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

        def reset_count(self):
            return 0

    monkeypatch.setattr(procedure.ambyte_serial, "AmbyteConsole", SlowConsole)
    monkeypatch.setattr(
        procedure.ambyte_serial, "esp_jtag_ports", lambda: ["/dev/ttyACM0"])

    console = procedure.ambyte_serial.connect_after_boot(
        "/dev/ttyACM0", deadline_s=30.0, settle_s=0.0)

    assert console is created[0]
    assert len(created) == 1
    assert console.polls == 3
    assert not console.closed


def test_onboarding_installs_selected_script_when_sd_has_no_identity(monkeypatch):
    script = LuaScriptRelease(
        tag="lua-v1.2.3",
        asset_name="main.lua",
        script_name="main",
        script_version="1.2.3",
        built_against_fw="1.7.0",
        asset_url="https://example.test/lua-v1.2.3/main.lua",
        sha256="a" * 64,
        size_bytes=10,
        campaign_id="lua-v1.2.3",
    )

    class FakeConsole:
        def __init__(self):
            self.status_reads = 0
            self.install_args = None
            self.close_count = 0

        def lua_release(self, timeout=10.0):
            self.status_reads += 1
            if self.status_reads == 1:
                raise ConsoleError("main.lua is absent")
            if self.status_reads == 2:
                raise ConsoleError("'lua release' got no prompt back within 20s")
            return LuaReleaseStatus(
                sha256="a" * 64,
                script_version="1.2.3",
                built_against_fw="1.7.0",
                installed_on_fw="1.7.1",
                verified=True,
                running=True,
            )

        def lua_install(self, *args):
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

    result = procedure.install_lua_script(
        SimpleNamespace(lua_script=script, wifi_ssid="test-ap"),
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

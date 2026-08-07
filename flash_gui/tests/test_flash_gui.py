"""Unit tests for the pure logic in flash_gui (no hardware, no network).

Run from the repo root:  python -m pytest flash_gui/tests -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from flash_gui import config                                   # noqa: E402
from flash_gui.ambyte_serial import expand_mac_token           # noqa: E402
from flash_gui.nvs_builder import (AMAZON_ROOT_CA1,            # noqa: E402
                                   NvsBuildError, ProvisioningPlan,
                                   build_nvs_csv, build_nvs_image)
from flash_gui.procedure import PreflightInfo, clean_device_name  # noqa: E402
from flash_gui.release_fetch import ReleaseError, ReleaseImages   # noqa: E402


MAC = "E8:F6:0A:B1:1F:34"
THING = "ambyte_E8:F6:0A:B1:1F:34"


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


# ── timezone helpers ─────────────────────────────────────────────────────────
def test_firmware_zone_table():
    from flash_gui.timezones import firmware_supports
    assert firmware_supports("Europe/Amsterdam")
    assert firmware_supports("UTC")
    assert not firmware_supports("America/Manaus")   # only on the tz-brazil branch

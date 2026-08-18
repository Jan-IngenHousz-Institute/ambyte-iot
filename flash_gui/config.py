# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Deployment constants + persisted session settings for the flash GUI.

Everything environment-shaped lives here so the rest of the tool never
hardcodes an endpoint or topic segment. Values that could not be determined
from the repos are marked TODO and must be filled before that path is used;
the GUI surfaces them as configuration errors rather than guessing.
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

APP_NAME = "ambyte-flash-gui"

# ── firmware source ──────────────────────────────────────────────────────────
# Canonical name is ambyte-iot: api.github.com/repos/<org>/protoMUSIC answers
# 301 to repository id 1192990170, whose full_name is ambyte-iot. The old name
# only resolves through GitHub's rename redirect, which dies the moment anyone
# creates a new repo called protoMUSIC, and pointing at it made an unrelated
# outage report name a repo that no longer exists.
# Release assets: ambyte-iot-v<X.Y.Z>.zip holding bootloader/, partition_table/,
# otadata, app and flasher_args.json (offsets come from that manifest, never
# hardcoded). nvs.bin is deliberately NOT in the asset (CI builds with
# AMBYTE_NVS_SKIP=1 so no secrets land in a public artifact); this tool bakes it
# per board.
FIRMWARE_REPO = os.environ.get("AMBYTE_FIRMWARE_REPO",
                               "Jan-IngenHousz-Institute/ambyte-iot")

# nvs @ 0x9000 size 0x6000, must match partitions.csv; the release's
# partition table is flashed alongside, so a drift would be a release bug.
NVS_OFFSET = 0x9000
NVS_PARTITION_SIZE = 0x6000

# littlefs @ 0x620000 size 0x80000 (partitions.csv): the firmware's internal
# script home. Provisioning bakes main.lua into an image flashed here so a
# fresh board runs the selected release without an SD seed step.
LITTLEFS_OFFSET = 0x620000
LITTLEFS_PARTITION_SIZE = 0x80000

# Espressif USB-Serial-JTAG (the native USB-C console/flash port).
USB_JTAG_VID = 0x303A
USB_JTAG_PID = 0x1001

# Firmware device_name buffer is char[64] -> 63 usable bytes.
MAX_NAME_LEN = 63


# ── openJII environments ─────────────────────────────────────────────────────
@dataclass(frozen=True)
class Environment:
    key: str
    api_url: str
    web_url: str
    # AWS IoT Core ATS data endpoint for this environment. The account-specific
    # prefix is not committed to the open-jii repo and no REST endpoint returns
    # it; ops resolve it with `aws iot describe-endpoint --endpoint-type
    # iot:Data-ATS`. Only the dev endpoint is publicly known.
    mqtt_uri: str | None

    @property
    def api_keys_url(self) -> str:
        # Where the user creates the personal API key the tool authenticates
        # with (better-auth apiKey plugin; the x-api-key header is accepted by
        # every /api/v1 endpoint). Locale segment required by the web app.
        return f"{self.web_url}/en-US/platform/account/api-keys"


ENVIRONMENTS: dict[str, Environment] = {
    "dev": Environment(
        key="dev",
        api_url="https://api.dev.openjii.org",
        web_url="https://dev.openjii.org",
        mqtt_uri="mqtts://a2s5vvyojsnl53-ats.iot.eu-central-1.amazonaws.com:8883",
    ),
    "prod": Environment(
        key="prod",
        api_url="https://api.openjii.org",
        web_url="https://openjii.org",
        mqtt_uri="mqtts://a3qrmjf5m5y241-ats.iot.eu-central-1.amazonaws.com:8883",
    ),
}

# The device family in the openJII registry. Upstream derives the Thing name
# as `<deviceType>_<serialNumber>` (sanitised; MAC colons survive), e.g.
# ambyte_E8:F6:0A:B1:1F:34, and the MQTT clientId MUST equal that Thing name
# (the IoT policy's identity-bound resources render as
# ${iot:Connection.Thing.ThingName}, which AWS only resolves when they match).
OPENJII_DEVICE_TYPE = "ambyte"


# ── MQTT topic conventions ───────────────────────────────────────────────────
# Canonical ingest grammar (open-jii asyncapi.yaml):
#   experiment/data_ingest/v1/{experimentId}/{sensorType}/{sensorVersion}/{sensorId}/{protocolId}
# The firmware stores the first 7 segments as mqtt_topic_root and appends
# protocol_id when publishing, so the rule's 8-level filter matches.
#
# sensorType/sensorVersion below follow what the deployed ambyte fleet already
# publishes (from the maintained provisioning .env) rather than the schema's
# suggestion, because Databricks' clean_data pipeline currently reads segment 5
# as protocol_id; changing these silently re-labels rows downstream.
TOPIC_SENSOR_TYPE = "multispeq"
TOPIC_SENSOR_VERSION = "v1.0"


def default_topic_root(experiment_id: str, thing_name: str) -> str:
    """The pre-filled (still user-editable) topic root for an experiment."""
    return (f"experiment/data_ingest/v1/{experiment_id}"
            f"/{TOPIC_SENSOR_TYPE}/{TOPIC_SENSOR_VERSION}/{thing_name}")


# Identity placeholder the GUI shows in the editable topic before a concrete
# board is known; replaced with the board's Thing name per procedure.
TOPIC_IDENTITY_TOKEN = "{thingName}"

# Cloud→device script channel: device/scripts/v1/{sensorType}/{sensorVersion}/
# {thingName}. Values mirror the deployed fleet's provisioning .env; the
# policy binds only the thingName segment.
def default_command_topic(thing_name: str) -> str:
    return f"device/scripts/v1/Ambyte/2/{thing_name}"


def default_status_topic(topic_root: str) -> str:
    # Replies must live under the same experiment as telemetry and carry a
    # trailing segment (the IoT policy grants publish on <root>/<seg>); the
    # /status leaf keeps replies off the telemetry leaf.
    return topic_root.rstrip("/") + "/status"


# NVS device-metadata defaults, mirroring the maintained provisioning .env.
# device_ver/device_firm are legacy junk strings the whole fleet reports; the
# real version telemetry comes from the compiled app descriptor.
DEVICE_ID = "03:25:07:04"
PROTOCOL_ID = "3517"
DEVICE_VERSION = "1"
DEVICE_FIRMWARE = "1"


# ── persisted session settings ───────────────────────────────────────────────
def _config_dir() -> Path:
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / APP_NAME
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / APP_NAME
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / APP_NAME


CONFIG_DIR = _config_dir()
SETTINGS_FILE = CONFIG_DIR / "settings.json"
# Release zips + unpacked images; safe to delete any time.
CACHE_DIR = CONFIG_DIR / "cache"
# Show-once credential bundles land here BEFORE any step that can fail:
# losing a private key means the board must be rotated again.
CERTS_DIR = CONFIG_DIR / "device_certs"


@dataclass
class Settings:
    """Session-level settings that persist across runs.

    The API keys authenticate as the operator; they are stored per environment
    with owner-only permissions (POSIX). Per-device values are deliberately NOT
    stored here.
    """

    environment: str = "dev"
    experiment_id: str = ""
    experiment_name: str = ""
    wifi_ssid: str = ""
    wifi_password: str = ""
    lua_script_name: str = "main"
    api_keys: dict = field(default_factory=dict)   # env key -> "jii_..." key

    @classmethod
    def load(cls) -> "Settings":
        try:
            blob = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return cls()
        known = {f for f in cls.__dataclass_fields__}
        return cls(**{k: v for k, v in blob.items() if k in known})

    def save(self) -> None:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        SETTINGS_FILE.write_text(
            json.dumps(self.__dict__, indent=2), encoding="utf-8")
        try:
            os.chmod(SETTINGS_FILE, 0o600)   # POSIX; Windows ACLs via profile
        except OSError:
            pass

    def api_key(self, env: str) -> str:
        return (self.api_keys or {}).get(env, "")

    def set_api_key(self, env: str, key: str) -> None:
        self.api_keys[env] = key

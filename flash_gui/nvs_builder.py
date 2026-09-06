# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Bake a per-board NVS provisioning image.

The firmware self-provisions at boot from the NVS partition (0x9000): identity,
MQTT config, timezone, Wi-Fi credentials, certificates, and a `flash_time` RTC
seed. Certificates and Wi-Fi credentials have NO console path at all (the cert
setters in components/certs are dead code; `wifi_join` does not set the
`wifi_prov/provisioned` flag), so baking NVS is the only complete provisioning
protocol — the console is used afterwards only for the exact RTC set and for
verification.

Field map mirrors tools/build_nvs_image.py in the firmware repo. Namespaces:
  device_cfg  mqtt_uri, mqtt_client_id, mqtt_topic_root, cmd_topic,
              status_topic, device_id, protocol_id, device_name, device_ver,
              device_firm, firmware_ver, timezone, site_state (atomic blob),
              flash_time (u32)
  certs       ca_cert, dev_cert, dev_key          (PEM strings, ≤2048 B each)
  wifi_creds  ssid, pass
  wifi_prov   provisioned (u8 = 1)

The image is generated with the vendored ESP-IDF nvs_partition_gen.py so the
tool needs no ESP-IDF install.
"""

from __future__ import annotations

import importlib.util
import io
import math
import time
from contextlib import redirect_stdout
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

from .config import NVS_PARTITION_SIZE
from tools.site_state_blob import encode_site_state

VENDOR_GEN = Path(__file__).resolve().parent / "vendor" / "nvs_partition_gen.py"

# Firmware cert buffers are CERT_BUF_SIZE 2048 (components/certs/certs.c) —
# a longer PEM would be truncated on device, which fails TLS in the field.
CERT_MAX_LEN = 2048

# Amazon Root CA 1 — AWS IoT never ships a root CA with issued credentials;
# this is the public root the whole repo tooling embeds.
AMAZON_ROOT_CA1 = """-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
"""


class NvsBuildError(RuntimeError):
    """Anything that prevents producing a valid NVS image."""


@dataclass
class ProvisioningPlan:
    """Everything one board gets baked into its NVS partition."""

    device_name: str
    timezone: str                 # IANA name, ≤47 chars on device
    mqtt_uri: str
    client_id: str                # MUST equal the openJII Thing name
    topic_root: str
    command_topic: str
    status_topic: str
    device_cert_pem: str
    device_key_pem: str
    wifi_ssid: str
    wifi_password: str
    device_id: str
    protocol_id: str
    device_version: str
    device_firmware: str
    firmware_version: str         # the release version being flashed
    ca_cert_pem: str = AMAZON_ROOT_CA1
    lat: float | None = None
    lon: float | None = None
    deployment: str | None = None
    # Schedule release provenance (script_upd namespace): pre-seeds the identity the
    # firmware would record after an on-device install, so a board flashed with
    # the littlefs schedule.yaml image verifies as already-installed. All-or-none.
    schedule_sha256: str | None = None
    schedule_script_version: str | None = None
    schedule_built_against_fw: str | None = None
    schedule_campaign_id: str | None = None

    def validate(self) -> None:
        required = {
            "device_name": self.device_name,
            "timezone": self.timezone,
            "mqtt_uri": self.mqtt_uri,
            "client_id": self.client_id,
            "topic_root": self.topic_root,
            "device_cert_pem": self.device_cert_pem,
            "device_key_pem": self.device_key_pem,
            "wifi_ssid": self.wifi_ssid,
        }
        missing = [k for k, v in required.items() if not (v or "").strip()]
        if missing:
            raise NvsBuildError("missing provisioning value(s): "
                                + ", ".join(missing))
        schedule = (self.schedule_sha256, self.schedule_script_version,
                    self.schedule_built_against_fw, self.schedule_campaign_id)
        if any(v is not None for v in schedule) and not all(
                (v or "").strip() for v in schedule):
            raise NvsBuildError("Schedule provenance is all-or-none: sha256, "
                                "script_version, built_against_fw, campaign_id")
        if self.schedule_sha256 is not None:
            sha = self.schedule_sha256.strip()
            if len(sha) != 64 or any(c not in "0123456789abcdefABCDEF"
                                     for c in sha):
                raise NvsBuildError("schedule_sha256 must be 64 hex digits")
        if len(self.timezone) > 47:
            raise NvsBuildError(f"timezone '{self.timezone}' exceeds the "
                                "firmware's 47-char buffer")
        if self.lat is not None and (not math.isfinite(self.lat)
                                     or not -90.0 <= self.lat <= 90.0):
            raise NvsBuildError("lat must be between -90 and 90")
        if self.lon is not None and (not math.isfinite(self.lon)
                                     or not -180.0 <= self.lon <= 180.0):
            raise NvsBuildError("lon must be between -180 and 180")
        if self.deployment is not None and len(
                self.deployment.encode("utf-8")) > 63:
            raise NvsBuildError("deployment must fit the firmware's 63-byte buffer")
        for label, pem in (("CA cert", self.ca_cert_pem),
                           ("device cert", self.device_cert_pem),
                           ("device key", self.device_key_pem)):
            if len(pem.encode("utf-8")) > CERT_MAX_LEN:
                raise NvsBuildError(
                    f"{label} is {len(pem)} bytes — exceeds the firmware's "
                    f"{CERT_MAX_LEN}-byte cert buffer")


def _quote_csv(value: str) -> str:
    """CSV-quote (RFC 4180) — PEM values contain newlines and commas."""
    return '"' + value.replace('"', '""') + '"'


def build_nvs_csv(plan: ProvisioningPlan, flash_time: int | None = None) -> str:
    """The nvs_partition_gen CSV for this plan.

    Namespaces are emitted once each — a repeated `namespace` row would start a
    SEPARATE namespace and hide keys from the firmware.
    """
    plan.validate()
    if flash_time is None:
        flash_time = int(time.time())

    lines = ["key,type,encoding,value"]

    def ns(name: str) -> None:
        lines.append(f"{name},namespace,,")

    def s(key: str, value: str) -> None:
        lines.append(f"{key},data,string,{_quote_csv(value)}")

    def u(key: str, enc: str, value: str) -> None:
        lines.append(f"{key},data,{enc},{value}")

    ns("device_cfg")
    s("mqtt_uri", plan.mqtt_uri)
    s("mqtt_client_id", plan.client_id)
    s("mqtt_topic_root", plan.topic_root)
    s("device_id", plan.device_id)
    s("protocol_id", plan.protocol_id)
    s("device_name", plan.device_name)
    s("device_ver", plan.device_version)
    s("device_firm", plan.device_firmware)
    s("firmware_ver", plan.firmware_version)
    s("timezone", plan.timezone)
    if (plan.lat is not None or plan.lon is not None
            or plan.deployment is not None):
        u("site_state", "hex2bin", encode_site_state(
            plan.lat, plan.lon, plan.deployment
        ).hex())
    if plan.command_topic:
        s("cmd_topic", plan.command_topic)
    if plan.status_topic:
        s("status_topic", plan.status_topic)
    # RTC seed: firmware adopts this at boot when the RTC is invalid or behind
    # it. The exact time is still set over the console afterwards; this floor
    # ensures a sane clock even if that step is skipped.
    u("flash_time", "u32", str(flash_time))

    ns("certs")
    s("ca_cert", plan.ca_cert_pem)
    s("dev_cert", plan.device_cert_pem)
    s("dev_key", plan.device_key_pem)

    ns("wifi_creds")
    s("ssid", plan.wifi_ssid)
    s("pass", plan.wifi_password)

    ns("wifi_prov")
    # Only this flag makes the firmware treat the board as provisioned;
    # console wifi_join deliberately does not set it.
    u("provisioned", "u8", "1")

    if plan.schedule_sha256 is not None:
        # Schedule release provenance for the baked-in littlefs schedule.yaml image
        # (components/script_update NVS keys): `schedule release` verifies the
        # flashed script against these, so the onboarding Schedule step is a no-op.
        ns("script_upd")
        s("applied_id", plan.schedule_campaign_id)
        s("script_sha", plan.schedule_sha256.lower())
        s("script_ver", plan.schedule_script_version)
        s("built_fw", plan.schedule_built_against_fw)
        s("install_fw", plan.firmware_version)

    return "\n".join(lines) + "\n"


_generator_module = None


def _load_generator():
    """Import the vendored generator from its file path, once.

    In-process on purpose: a PyInstaller-frozen app has no separate Python
    interpreter to shell out to (sys.executable IS the frozen exe), so a
    subprocess call would re-launch the GUI instead of the generator.
    """
    global _generator_module
    if _generator_module is not None:
        return _generator_module
    if not VENDOR_GEN.is_file():
        raise NvsBuildError(f"vendored NVS generator missing: {VENDOR_GEN}")
    spec = importlib.util.spec_from_file_location("nvs_partition_gen",
                                                  VENDOR_GEN)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _generator_module = module
    return module


def build_nvs_image(plan: ProvisioningPlan, out_path: Path,
                    flash_time: int | None = None) -> Path:
    """Generate the NVS binary at out_path (NVS_PARTITION_SIZE bytes)."""
    gen = _load_generator()
    out_path = Path(out_path).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    csv_path = out_path.with_suffix(".csv")
    csv_path.write_text(build_nvs_csv(plan, flash_time), encoding="utf-8")

    # Mirrors the `generate` sub-command's argparse defaults; the generator
    # signals bad input with sys.exit(), which surfaces here as SystemExit.
    args = SimpleNamespace(input=str(csv_path), output=str(out_path),
                           size=hex(NVS_PARTITION_SIZE),
                           outdir=str(out_path.parent), version=2)
    quiet = io.StringIO()
    try:
        with redirect_stdout(quiet):
            gen.generate(args)
    except SystemExit as exc:
        raise NvsBuildError(f"nvs_partition_gen rejected the input "
                            f"(exit {exc.code}):\n{quiet.getvalue()}") from exc
    except Exception as exc:
        raise NvsBuildError(f"nvs_partition_gen failed: {exc}\n"
                            f"{quiet.getvalue()}") from exc
    if not out_path.is_file() or out_path.stat().st_size != NVS_PARTITION_SIZE:
        raise NvsBuildError("nvs_partition_gen produced no/short image")
    return out_path

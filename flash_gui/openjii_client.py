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
from dataclasses import dataclass
from pathlib import Path

from .config import CERTS_DIR, OPENJII_DEVICE_TYPE, Environment

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


@dataclass
class DeviceIdentity:
    """What provisioning needs from the registry + credential issuance."""

    device_id: str
    thing_name: str        # MQTT client id MUST equal this
    certificate_pem: str
    private_key_pem: str
    bundle_dir: Path       # where the show-once PEMs were persisted
    rotated: bool


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
            with urllib.request.urlopen(req, timeout=timeout) as resp:
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

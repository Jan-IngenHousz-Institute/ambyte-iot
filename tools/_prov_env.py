"""Shared env/bundle helpers for host-side provisioning tooling.

Two jobs:
  1. Auto-load .env from the repo root into os.environ (setdefault — shell env wins).
  2. Resolve a "cert bundle" into AMBYTE_CA_CERT / AMBYTE_DEV_CERT / AMBYTE_DEV_KEY.

Bundle layout (a manually-assembled bundle or an AWS IoT download):
    device_certs/
        <bundle-name>/
            *RootCA*.pem                       (optional; AWS downloads ship none —
                                                falls back to the repo CA / embedded)
            *-certificate.pem.crt | *.cert.pem (device cert)
            *-private.pem.key | *.private.key  (device key)

Bundle selection precedence:
  1. AMBYTE_CERT_BUNDLE env var / .env entry
  2. If exactly one subfolder under device_certs/, pick it silently
  3. Interactive prompt listing available bundles (stdin must be a TTY)

Explicit AMBYTE_CA_CERT / AMBYTE_DEV_CERT / AMBYTE_DEV_KEY always override
bundle auto-resolution for their respective slot.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

REPO_ROOT    = Path(__file__).resolve().parent.parent
BUNDLES_DIR  = REPO_ROOT / "device_certs"
DOTENV_PATH  = REPO_ROOT / ".env"

# Repo CA fallback: AWS IoT bundles ship no root CA, so reuse the copy that
# already lives beside an earlier bundle (kept in sync with flash_certs.py).
_REPO_CA = BUNDLES_DIR / "dom_ludo_prototype_ambyte_thing_v2" / "AmazonRootCA1.pem"

# Amazon Root CA 1 — last-resort embedded copy so provisioning is self-contained
# even in a checkout with no repo CA. Written into the bundle when nothing else
# is found. Same public root as flash_certs.py.
_AMAZON_ROOT_CA1 = """-----BEGIN CERTIFICATE-----
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


def load_dotenv() -> None:
    """Populate os.environ with KEY=VALUE lines from `.env`. Existing env wins."""
    if not DOTENV_PATH.exists():
        return
    for raw in DOTENV_PATH.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def _pick_bundle_interactively(subs: list[Path]) -> str | None:
    if not sys.stdin.isatty():
        return None
    print("Multiple cert bundles under device_certs/ — pick one:", file=sys.stderr)
    for i, s in enumerate(subs):
        print(f"  [{i}] {s.name}", file=sys.stderr)
    choice = input("bundle (name or index): ").strip()
    if choice.isdigit() and 0 <= int(choice) < len(subs):
        return subs[int(choice)].name
    if any(s.name == choice for s in subs):
        return choice
    print(f"warning: no bundle matches {choice!r}", file=sys.stderr)
    return None


def _find(files: list[Path], predicate) -> str | None:
    for f in files:
        if predicate(f.name):
            return str(f.resolve().relative_to(REPO_ROOT))
    return None


def resolve_cert_bundle() -> None:
    """Fill AMBYTE_CA_CERT/AMBYTE_DEV_CERT/AMBYTE_DEV_KEY from a bundle directory.
    No-op if the three are already all set. Any already-set ones are preserved."""
    if (os.environ.get("AMBYTE_CA_CERT")
        and os.environ.get("AMBYTE_DEV_CERT")
        and os.environ.get("AMBYTE_DEV_KEY")):
        return
    if not BUNDLES_DIR.is_dir():
        return

    bundle = os.environ.get("AMBYTE_CERT_BUNDLE")
    if not bundle:
        subs = sorted(d for d in BUNDLES_DIR.iterdir() if d.is_dir())
        if len(subs) == 1:
            bundle = subs[0].name
        elif len(subs) > 1:
            bundle = _pick_bundle_interactively(subs)
        if not bundle:
            return

    bundle_dir = BUNDLES_DIR / bundle
    if not bundle_dir.is_dir():
        print(f"warning: AMBYTE_CERT_BUNDLE={bundle!r} — {bundle_dir} not found", file=sys.stderr)
        return

    os.environ["AMBYTE_CERT_BUNDLE"] = bundle

    # AWS IoT client_id must match the thing the cert is bound to — for a
    # manually-assembled bundle the folder name is that thing name, so default
    # client_id to it and warn on mismatch. A client_id carrying a {MAC}-style
    # token is a per-device identity resolved on-device (the folder name is then
    # just the AWS cert-id, not a thing name), so the equality check doesn't
    # apply — skip the warning to avoid a false positive.
    existing_client_id = os.environ.get("AMBYTE_CLIENT_ID")
    if (existing_client_id and existing_client_id != bundle
            and "{" not in existing_client_id):
        print(
            f"warning: AMBYTE_CLIENT_ID={existing_client_id!r} does not match "
            f"AMBYTE_CERT_BUNDLE={bundle!r}. AWS IoT will reject the handshake "
            f"if the cert isn't bound to this client id.",
            file=sys.stderr)
    os.environ.setdefault("AMBYTE_CLIENT_ID", bundle)

    files = sorted(bundle_dir.iterdir())

    # Device cert: AWS downloads name it "<id>.cert.pem"; the older manual bundles
    # use "<id>-certificate.pem.crt". Accept both (and a bare ".crt").
    cert = _find(files, lambda n: n.endswith("-certificate.pem.crt")
                                   or n.endswith(".cert.pem")
                                   or n.endswith(".crt"))
    key  = _find(files, lambda n: n.endswith("-private.pem.key")
                                   or (n.endswith(".key") and "private" in n.lower())
                                   or n.endswith(".key"))

    # CA: prefer one shipped in the bundle. AWS IoT downloads ship none, so fall
    # back to the repo CA, then to the embedded root — mirroring flash_certs.py.
    # (Exclude the ".cert.pem" device cert from the fuzzy "*ca*.pem" match.)
    ca = _find(files, lambda n: "rootca" in n.lower().replace("_", "").replace("-", ""))
    if not ca:
        ca = _find(files, lambda n: n.lower().endswith(".pem")
                                     and "ca" in n.lower()
                                     and not n.lower().endswith(".cert.pem"))
    if not ca and _REPO_CA.is_file():
        ca = str(_REPO_CA.resolve().relative_to(REPO_ROOT))
    if not ca:
        bundle_ca = bundle_dir / "AmazonRootCA1.pem"
        bundle_ca.write_text(_AMAZON_ROOT_CA1, encoding="utf-8")
        ca = str(bundle_ca.resolve().relative_to(REPO_ROOT))
        print(f"note: bundle ships no CA — wrote embedded Amazon Root CA 1 "
              f"-> {ca}", file=sys.stderr)

    if ca:   os.environ.setdefault("AMBYTE_CA_CERT",  ca)
    if cert: os.environ.setdefault("AMBYTE_DEV_CERT", cert)
    if key:  os.environ.setdefault("AMBYTE_DEV_KEY",  key)

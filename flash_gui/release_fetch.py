"""Resolve the firmware images to flash from the repo's GitHub releases.

Adapted from the maintained flash-bundle logic: the release asset
`ambyte-iot-v<X.Y.Z>.zip` is CI-built from the released commit and carries
`flasher_args.json`, the build's own manifest — offsets, flash mode/size/freq,
chip and reset behaviour are read FROM IT rather than hardcoded, so a future
partition-table move cannot silently write the app to the wrong address.
`nvs.bin` is deliberately absent from the asset (no secrets in a public
artifact); the caller bakes it per board and splices it in at NVS_OFFSET.

Assets are cached under the user config dir keyed by tag + byte size, so a
session with 10 boards downloads once. Everything raises ReleaseError instead
of exiting — the GUI turns that into a message.
"""

from __future__ import annotations

import json
import os
import shutil
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

from .config import CACHE_DIR, FIRMWARE_REPO

API_ROOT = "https://api.github.com"
ASSET_PREFIX = "ambyte-iot-v"
ASSET_SUFFIX = ".zip"
MANIFEST = "flasher_args.json"
# Written next to the unpacked images so a half-finished download is never
# mistaken for a good cache entry.
STAMP = ".complete.json"
USER_AGENT = "ambyte-flash-gui"


class ReleaseError(RuntimeError):
    """Anything that prevents resolving a flashable release."""


def _auth_headers() -> dict[str, str]:
    # Public repo needs no auth, but unauthenticated GitHub allows only 60
    # requests/hour per IP; GH_TOKEN/GITHUB_TOKEN raises that and costs nothing
    # when absent.
    headers = {"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def _get_json(url: str) -> dict:
    req = urllib.request.Request(url, headers=_auth_headers())
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            detail = exc.read().decode("utf-8", errors="replace")[:200]
        except Exception:
            pass
        if exc.code == 403 and "rate limit" in detail.lower():
            raise ReleaseError(
                "GitHub rate limit hit. Set GH_TOKEN to a personal access "
                "token and restart, or reuse an already-cached release.") from exc
        raise ReleaseError(f"GitHub API {exc.code} for {url}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise ReleaseError(f"cannot reach GitHub ({exc.reason})") from exc


class ReleaseImages:
    """The resolved set of images to write, plus where they came from.

    `flash_files` is (offset, path) ordered low→high, exactly as esptool wants
    it, and NVS is not in it — the caller splices its freshly-baked nvs.bin in.
    """

    def __init__(self, tag: str, version: str, root: Path, manifest: dict):
        self.tag = tag
        self.version = version
        self.root = root
        self.manifest = manifest

    @property
    def flash_files(self) -> list[tuple[int, Path]]:
        files = self.manifest.get("flash_files") or {}
        if not files:
            raise ReleaseError(f"{MANIFEST} in {self.tag} has no flash_files.")
        out: list[tuple[int, Path]] = []
        for off, rel in files.items():
            path = self.root / rel
            if not path.is_file():
                raise ReleaseError(
                    f"{self.tag} manifest lists {rel} but the file is missing.")
            out.append((int(off, 16), path))
        out.sort(key=lambda item: item[0])
        return out

    @property
    def flash_settings(self) -> dict[str, str]:
        settings = self.manifest.get("flash_settings") or {}
        return {
            "flash_mode": settings.get("flash_mode", "dio"),
            "flash_size": settings.get("flash_size", "16MB"),
            "flash_freq": settings.get("flash_freq", "80m"),
        }

    @property
    def chip(self) -> str:
        return (self.manifest.get("extra_esptool_args") or {}).get("chip", "esp32s3")


def fetch_latest(log=print) -> ReleaseImages:
    """Download (or reuse from cache) the newest published firmware release.

    /releases/latest excludes drafts and prereleases on purpose: a bench rc
    must never reach a field board by default.
    """
    release = _get_json(f"{API_ROOT}/repos/{FIRMWARE_REPO}/releases/latest")
    tag = release.get("tag_name") or "(untagged)"

    asset = None
    for cand in release.get("assets", []):
        name = cand.get("name", "")
        if name.startswith(ASSET_PREFIX) and name.endswith(ASSET_SUFFIX):
            asset = cand
            break
    if asset is None:
        raise ReleaseError(
            f"release {tag} has no {ASSET_PREFIX}*{ASSET_SUFFIX} asset — it may "
            "be a Lua-only release; a firmware release is required.")

    dest = CACHE_DIR / "releases" / tag
    stamp = dest / STAMP
    expect = asset.get("size")

    cached = False
    if stamp.is_file():
        try:
            prev = json.loads(stamp.read_text(encoding="utf-8"))
            cached = (prev.get("name") == asset.get("name")
                      and prev.get("size") == expect)
        except Exception:
            cached = False

    if not cached:
        if dest.exists():
            shutil.rmtree(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        zip_path = dest.parent / f"{tag}-{asset['name']}"
        log(f"Downloading {asset['name']} ({tag})...")
        _download(asset["browser_download_url"], zip_path, expect)
        with zipfile.ZipFile(zip_path) as zf:
            _safe_extract(zf, dest)
        zip_path.unlink(missing_ok=True)
        stamp.write_text(json.dumps({"name": asset["name"], "size": expect,
                                     "tag": tag}), encoding="utf-8")
    else:
        log(f"Firmware {tag} already cached.")

    manifest_path = dest / MANIFEST
    if not manifest_path.is_file():
        raise ReleaseError(
            f"{tag} asset has no {MANIFEST} — cannot determine flash offsets.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = tag[1:] if tag.startswith("v") else tag
    return ReleaseImages(tag, version, dest, manifest)


def _download(url: str, dest: Path, expect_size: int | None) -> None:
    req = urllib.request.Request(url, headers={**_auth_headers(),
                                               "Accept": "application/octet-stream"})
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    try:
        with urllib.request.urlopen(req, timeout=120) as resp, tmp.open("wb") as out:
            shutil.copyfileobj(resp, out)
    except urllib.error.URLError as exc:
        tmp.unlink(missing_ok=True)
        raise ReleaseError(f"downloading {url} failed: {exc}") from exc
    got = tmp.stat().st_size
    if expect_size and got != expect_size:
        tmp.unlink(missing_ok=True)
        raise ReleaseError(f"asset truncated: got {got} bytes, expected {expect_size}.")
    tmp.replace(dest)


def _safe_extract(zf: zipfile.ZipFile, dest: Path) -> None:
    # The zip is ours and CI-built, so this is belt-and-braces — but an
    # extraction that can write outside its directory only bites once.
    dest = dest.resolve()
    for member in zf.infolist():
        target = (dest / member.filename).resolve()
        if not str(target).startswith(str(dest)):
            raise ReleaseError(f"zip member escapes the cache: {member.filename}")
    zf.extractall(dest)

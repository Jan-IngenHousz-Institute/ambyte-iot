# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Resolve the firmware images to flash from the repo's GitHub releases.

Adapted from the maintained flash-bundle logic: the release asset
`ambyte-iot-v<X.Y.Z>.zip` is CI-built from the released commit and carries
`flasher_args.json`, the build's own manifest: offsets, flash mode/size/freq,
chip and reset behaviour are read FROM IT rather than hardcoded, so a future
partition-table move cannot silently write the app to the wrong address.
`nvs.bin` is deliberately absent from the asset (no secrets in a public
artifact); the caller bakes it per board and splices it in at NVS_OFFSET.

Assets are cached under the user config dir keyed by tag + byte size, so a
session with 10 boards downloads once. Everything raises ReleaseError instead
of exiting; the GUI turns that into a message.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import time
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .config import CACHE_DIR, FIRMWARE_REPO
from .tls import ssl_context

API_ROOT = "https://api.github.com"
ASSET_PREFIX = "ambyte-iot-v"
ASSET_SUFFIX = ".zip"
LUA_TAG_RE = re.compile(r"^lua-v(\d+)\.(\d+)\.(\d+)$")
LUA_ASSET_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*\.lua$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MANIFEST = "flasher_args.json"
# Written next to the unpacked images so a half-finished download is never
# mistaken for a good cache entry.
STAMP = ".complete.json"
USER_AGENT = "ambyte-flash-gui"
# The firmware and Lua fetchers both want this exact listing, so it is fetched
# once and shared: unauthenticated GitHub allows 60 requests/hour per IP, and a
# NATed office shares one budget across every operator.
RELEASES_URL = f"{API_ROOT}/repos/{FIRMWARE_REPO}/releases?per_page=100"
RELEASES_CACHE = CACHE_DIR / "releases.json"
# Release assets are immutable (the repo publishes immutable releases), so a
# manifest fetched once for a given URL can never change. Caching them forever
# is what lets a warm PC bootstrap a whole onboarding session offline.
MANIFEST_CACHE = CACHE_DIR / "lua_manifests.json"
# Short enough that a release published mid-session is picked up, long enough
# to collapse the two startup fetches into one request.
RELEASES_TTL_SECONDS = 300


class ReleaseError(RuntimeError):
    """Anything that prevents resolving a flashable release."""


@dataclass(frozen=True)
class LuaScriptRelease:
    """One immutable script choice from a published Lua catalog release."""

    tag: str
    asset_name: str
    script_name: str
    script_version: str
    built_against_fw: str
    asset_url: str
    sha256: str
    size_bytes: int
    campaign_id: str


@dataclass(frozen=True)
class LuaCatalogRelease:
    """The newest stable lua-v* release and its validated script choices."""

    tag: str
    scripts: tuple[LuaScriptRelease, ...]


def pick_firmware_release(releases: list) -> dict | None:
    """The newest published FIRMWARE release from a /releases listing.

    The repo's release stream carries more than firmware: lua-v* script
    releases and flash-gui-v* tool releases share it, and GitHub's
    /releases/latest simply returns the most recently published one, and the day
    flash-gui-v0.1.0 was tagged, "latest" stopped carrying a firmware zip and
    every flasher in the field broke. So: scan the list, keep only non-draft,
    non-prerelease entries whose tag is a plain firmware `vX.Y.Z` AND that
    actually ship the flash-bundle asset, and take the newest by publish date.
    """
    def is_firmware(rel: dict) -> bool:
        if rel.get("draft") or rel.get("prerelease"):
            return False
        tag = rel.get("tag_name") or ""
        if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
            return False
        return any(
            (a.get("name") or "").startswith(ASSET_PREFIX)
            and (a.get("name") or "").endswith(ASSET_SUFFIX)
            for a in rel.get("assets") or [])

    candidates = [r for r in releases if isinstance(r, dict) and is_firmware(r)]
    if not candidates:
        return None
    return max(candidates, key=lambda r: r.get("published_at") or "")


def pick_lua_release(releases: list) -> dict | None:
    """The newest stable lua-v* release containing a usable script catalog.

    A catalog entry is a plain ``*.lua`` asset plus its adjacent
    ``*.lua.manifest.json`` contract. Releases of other units, prereleases,
    drafts, and incomplete Lua releases are ignored.
    """

    def is_lua_catalog(rel: dict) -> bool:
        if rel.get("draft") or rel.get("prerelease"):
            return False
        if LUA_TAG_RE.fullmatch(rel.get("tag_name") or "") is None:
            return False
        names = {
            asset.get("name")
            for asset in rel.get("assets") or []
            if isinstance(asset, dict)
        }
        return any(
            isinstance(name, str)
            and LUA_ASSET_RE.fullmatch(name)
            and f"{name}.manifest.json" in names
            for name in names
        )

    candidates = [r for r in releases if isinstance(r, dict) and is_lua_catalog(r)]
    if not candidates:
        return None
    return max(
        candidates,
        key=lambda r: tuple(
            int(part) for part in LUA_TAG_RE.fullmatch(r["tag_name"]).groups()),
    )


def _auth_headers() -> dict[str, str]:
    # Public repo needs no auth, but unauthenticated GitHub allows only 60
    # requests/hour per IP; GH_TOKEN/GITHUB_TOKEN raises that and costs nothing
    # when absent.
    headers = {"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def _rate_limit_message(headers: Any, detail: str = "") -> str | None:
    """Operator-readable rate-limit text, or None if this is a different error.

    The old message only said "set GH_TOKEN", which is useless to an operator
    mid-session who will not stop to mint a PAT. GitHub reports when the window
    clears, so say it: waiting is usually the answer.
    """
    remaining = None
    reset = None
    limit = None
    if headers is not None:
        remaining = headers.get("X-RateLimit-Remaining")
        reset = headers.get("X-RateLimit-Reset")
        limit = headers.get("X-RateLimit-Limit")
    if remaining != "0" and "rate limit" not in detail.lower():
        return None

    when = ""
    try:
        when = time.strftime("%H:%M", time.localtime(int(reset)))
    except (TypeError, ValueError):
        pass
    return (
        f"GitHub rate limit reached ({limit or 60} requests/hour, shared by "
        f"everyone on this network)."
        + (f" It resets at {when}." if when else "")
        + " Wait for the reset, or set GH_TOKEN to a GitHub personal access "
        "token (no scopes needed for a public repo) and restart."
    )


def _get_json(url: str) -> Any:
    req = urllib.request.Request(url, headers=_auth_headers())
    try:
        with urllib.request.urlopen(
                req, timeout=30, context=ssl_context()) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            detail = exc.read().decode("utf-8", errors="replace")[:200]
        except Exception:
            pass
        if exc.code in (403, 429):
            limited = _rate_limit_message(getattr(exc, "headers", None), detail)
            if limited:
                raise ReleaseError(limited) from exc
        raise ReleaseError(f"GitHub API {exc.code} for {url}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise ReleaseError(f"cannot reach GitHub ({exc.reason})") from exc


def _read_cache_file(path: Path) -> Any:
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return None


def _write_cache_file(path: Path, payload: Any) -> None:
    # A cache we cannot write is not a reason to fail a flash.
    try:
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)
        tmp.replace(path)
    except OSError:
        pass


def _cached_manifest(url: str) -> Any:
    """A Lua release manifest, fetched at most once per URL, ever.

    Falls back to the stored copy when GitHub is unreachable, so a bench that
    has already seen this release keeps working with no connectivity.
    """
    store = _read_cache_file(MANIFEST_CACHE)
    store = store if isinstance(store, dict) else {}
    if url in store:
        return store[url]
    manifest = _get_json(url)
    store[url] = manifest
    _write_cache_file(MANIFEST_CACHE, store)
    return manifest


def _load_releases_cache() -> dict | None:
    cached = _read_cache_file(RELEASES_CACHE)
    if isinstance(cached, dict) and isinstance(cached.get("releases"), list):
        return cached
    return None


def _store_releases_cache(releases: list, etag: str | None) -> None:
    _write_cache_file(RELEASES_CACHE,
                      {"fetched_at": time.time(), "etag": etag,
                       "releases": releases})


def _release_list(log=print) -> list:
    """The repo's newest 100 releases, shared by both fetchers.

    Costs at most one API request per RELEASES_TTL_SECONDS, and usually zero:
    past the TTL the listing is revalidated with If-None-Match, and GitHub does
    not count a 304 against the rate limit. When GitHub is unreachable or the
    limit is already spent, a stale cache beats refusing to flash.
    """
    cached = _load_releases_cache()
    age = time.time() - (cached or {}).get("fetched_at", 0)
    if cached and 0 <= age < RELEASES_TTL_SECONDS:
        return cached["releases"]

    headers = _auth_headers()
    if cached and cached.get("etag"):
        headers["If-None-Match"] = cached["etag"]

    def fall_back(reason: str) -> list:
        if not cached:
            raise ReleaseError(reason)
        # A missing or implausible fetched_at must not render as "cached
        # 29782919 min ago"; say nothing rather than something absurd.
        if 0 <= age < 86400:
            when = f"cached {int(age // 60)} min ago"
        elif 0 <= age < 86400 * 30:
            when = f"cached {int(age // 86400)} day(s) ago"
        else:
            when = "previously cached"
        log(f"{reason} Using the {when} release list.")
        return cached["releases"]

    req = urllib.request.Request(RELEASES_URL, headers=headers)
    try:
        with urllib.request.urlopen(
                req, timeout=30, context=ssl_context()) as resp:
            releases = json.loads(resp.read().decode("utf-8"))
            if isinstance(releases, list) and releases:
                _store_releases_cache(releases, resp.headers.get("ETag"))
                return releases
            # HTTP 200 with [] is what GitHub serves during a releases-API
            # incident: on 2026-08-17 every repository on github.com returned an
            # empty list for an hour while /releases/latest and GraphQL stayed
            # correct. It is indistinguishable from a repo that has no releases,
            # so it must never overwrite a cache holding real entries, and it
            # must not be reported as "no firmware release found".
            return fall_back(
                "GitHub returned an empty release list, which normally means an "
                "API incident rather than a repository without releases (check "
                "https://www.githubstatus.com).")
    except urllib.error.HTTPError as exc:
        if exc.code == 304 and cached:
            # Unchanged, and free: 304s do not count against the rate limit.
            # Re-stamp so the TTL restarts instead of revalidating every call.
            _store_releases_cache(cached["releases"], cached.get("etag"))
            return cached["releases"]
        detail = ""
        try:
            detail = exc.read().decode("utf-8", errors="replace")[:200]
        except Exception:
            pass
        limited = (_rate_limit_message(getattr(exc, "headers", None), detail)
                   if exc.code in (403, 429) else None)
        return fall_back(limited or f"GitHub API {exc.code}: {detail}")
    except urllib.error.URLError as exc:
        return fall_back(f"Cannot reach GitHub ({exc.reason}).")


def _validated_lua_script(tag: str, asset: dict, manifest: Any) -> LuaScriptRelease:
    """Validate one release asset against its immutable schema-1 manifest."""
    asset_name = asset.get("name") or ""
    if LUA_ASSET_RE.fullmatch(asset_name) is None:
        raise ReleaseError(f"invalid Lua release asset name: {asset_name!r}")
    if not isinstance(manifest, dict):
        raise ReleaseError(f"{asset_name}.manifest.json is not a JSON object")

    version = tag.removeprefix("lua-v")
    script_name = asset_name.removesuffix(".lua")
    campaign_id = tag if script_name == "main" else f"{tag}:{script_name}"
    asset_url = asset.get("browser_download_url") or ""
    sha256 = manifest.get("sha256")
    size_bytes = manifest.get("size_bytes")
    built_against_fw = manifest.get("built_against_fw")
    command = manifest.get("script_update")

    checks = [
        (manifest.get("schema_version") == 1, "unsupported manifest schema"),
        (manifest.get("tag") == tag, "manifest tag does not match the release"),
        (manifest.get("script_name") == script_name,
         "manifest script_name does not match the asset"),
        (manifest.get("script_version") == version,
         "manifest script_version does not match the release"),
        (isinstance(built_against_fw, str) and bool(built_against_fw),
         "manifest built_against_fw is missing"),
        (isinstance(sha256, str) and SHA256_RE.fullmatch(sha256) is not None,
         "manifest sha256 is invalid"),
        (isinstance(size_bytes, int) and size_bytes >= 0,
         "manifest size_bytes is invalid"),
        (size_bytes == asset.get("size"),
         "manifest size_bytes does not match the release asset"),
        (manifest.get("asset_url") == asset_url,
         "manifest asset_url does not match the release asset"),
        (isinstance(command, dict), "manifest script_update is missing"),
    ]
    for ok, detail in checks:
        if not ok:
            raise ReleaseError(f"{asset_name}: {detail}")

    expected_command = {
        "type": "script_update",
        "id": campaign_id,
        "url": asset_url,
        "checksum": sha256,
        "script_version": version,
        "built_against_fw": built_against_fw,
    }
    if command != expected_command:
        raise ReleaseError(
            f"{asset_name}: manifest script_update does not match its release identity")

    return LuaScriptRelease(
        tag=tag,
        asset_name=asset_name,
        script_name=script_name,
        script_version=version,
        built_against_fw=built_against_fw,
        asset_url=asset_url,
        sha256=sha256,
        size_bytes=size_bytes,
        campaign_id=campaign_id,
    )


def fetch_latest_lua_catalog(log=print) -> LuaCatalogRelease:
    """Resolve and validate every selectable script in the latest Lua release."""
    releases = _release_list(log=log)
    release = pick_lua_release(releases if isinstance(releases, list) else [])
    if release is None:
        raise ReleaseError(
            f"no stable lua-vX.Y.Z catalog with .lua assets and manifests found "
            f"among the newest 100 releases of {FIRMWARE_REPO}.")

    tag = release["tag_name"]
    assets = {
        asset.get("name"): asset
        for asset in release.get("assets") or []
        if isinstance(asset, dict) and asset.get("name")
    }
    scripts: list[LuaScriptRelease] = []
    for asset_name in sorted(
            name for name in assets if LUA_ASSET_RE.fullmatch(name)):
        manifest_asset = assets.get(f"{asset_name}.manifest.json")
        if manifest_asset is None:
            raise ReleaseError(
                f"{tag} asset {asset_name} has no companion manifest")
        manifest_url = manifest_asset.get("browser_download_url") or ""
        manifest = _cached_manifest(manifest_url)
        scripts.append(_validated_lua_script(tag, assets[asset_name], manifest))

    if not scripts:
        raise ReleaseError(f"{tag} contains no selectable Lua scripts")
    log(f"Lua catalog {tag}: {len(scripts)} released script(s).")
    return LuaCatalogRelease(tag=tag, scripts=tuple(scripts))


class ReleaseImages:
    """The resolved set of images to write, plus where they came from.

    `flash_files` is (offset, path) ordered low→high, exactly as esptool wants
    it, and NVS is not in it, so the caller splices its freshly-baked nvs.bin in.
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

    Deliberately NOT /releases/latest: that endpoint returns the most recently
    published release of ANY kind, and this repo also publishes lua-v* and
    flash-gui-v* releases. pick_firmware_release() filters to real firmware
    releases (vX.Y.Z + flash-bundle asset, no drafts/prereleases).
    """
    releases = _release_list(log=log)
    release = pick_firmware_release(releases if isinstance(releases, list)
                                    else [])
    if release is None:
        raise ReleaseError(
            f"no firmware release (vX.Y.Z with an {ASSET_PREFIX}*{ASSET_SUFFIX} "
            f"asset) found among the newest 100 releases of {FIRMWARE_REPO}.")
    tag = release.get("tag_name") or "(untagged)"

    asset = next(
        cand for cand in release.get("assets", [])
        if (cand.get("name") or "").startswith(ASSET_PREFIX)
        and (cand.get("name") or "").endswith(ASSET_SUFFIX))

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
            f"{tag} asset has no {MANIFEST}, cannot determine flash offsets.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = tag[1:] if tag.startswith("v") else tag
    return ReleaseImages(tag, version, dest, manifest)


def _download(url: str, dest: Path, expect_size: int | None) -> None:
    req = urllib.request.Request(url, headers={**_auth_headers(),
                                               "Accept": "application/octet-stream"})
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    try:
        with urllib.request.urlopen(
                req, timeout=120, context=ssl_context()) as resp, \
                tmp.open("wb") as out:
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
    # The zip is ours and CI-built, so this is belt-and-braces, but an
    # extraction that can write outside its directory only bites once.
    dest = dest.resolve()
    for member in zf.infolist():
        target = (dest / member.filename).resolve()
        if not str(target).startswith(str(dest)):
            raise ReleaseError(f"zip member escapes the cache: {member.filename}")
    zf.extractall(dest)

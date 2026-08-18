# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The shared release listing: one request per TTL, free revalidation.

Unauthenticated GitHub allows 60 requests/hour per IP, shared by every operator
behind an office NAT, and the flasher used to spend two of them per start on the
same URL.
"""

import io
import json
import time
import urllib.error

import pytest

from flash_gui import release_fetch
from flash_gui.release_fetch import ReleaseError


class _Resp(io.BytesIO):
    def __init__(self, payload, etag="W/\"abc\""):
        super().__init__(json.dumps(payload).encode())
        self.headers = {"ETag": etag}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def _http_error(code, headers=None, body=b"{}"):
    return urllib.error.HTTPError(
        release_fetch.RELEASES_URL, code, "err", headers or {}, io.BytesIO(body))


def _count_calls(monkeypatch, responder):
    calls = []

    def fake_urlopen(req, **kwargs):
        calls.append(req)
        return responder(req)

    monkeypatch.setattr(release_fetch.urllib.request, "urlopen", fake_urlopen)
    return calls


def test_both_fetchers_share_a_single_request(monkeypatch):
    calls = _count_calls(monkeypatch, lambda req: _Resp([{"tag_name": "v1.0.0"}]))
    first = release_fetch._release_list(log=lambda _m: None)
    second = release_fetch._release_list(log=lambda _m: None)
    assert first == second == [{"tag_name": "v1.0.0"}]
    # The firmware and Lua fetchers used to cost one request each.
    assert len(calls) == 1


def test_stale_cache_is_revalidated_with_if_none_match(monkeypatch):
    _count_calls(monkeypatch, lambda req: _Resp([{"tag_name": "v1.0.0"}]))
    release_fetch._release_list(log=lambda _m: None)

    # Age the cache past the TTL.
    cached = json.loads(release_fetch.RELEASES_CACHE.read_text())
    cached["fetched_at"] = time.time() - release_fetch.RELEASES_TTL_SECONDS - 1
    release_fetch.RELEASES_CACHE.write_text(json.dumps(cached))

    def not_modified(req):
        assert req.get_header("If-none-match") == 'W/"abc"'
        raise _http_error(304)

    calls = _count_calls(monkeypatch, not_modified)
    # A 304 is not counted against the rate limit, and still serves the data.
    assert release_fetch._release_list(log=lambda _m: None) == [{"tag_name": "v1.0.0"}]
    assert len(calls) == 1

    # The 304 re-stamps the cache, so the next call is free rather than a
    # revalidation on every single call once stale.
    calls = _count_calls(monkeypatch, lambda req: pytest.fail("should not refetch"))
    assert release_fetch._release_list(log=lambda _m: None) == [{"tag_name": "v1.0.0"}]
    assert calls == []


def test_rate_limit_falls_back_to_stale_cache_rather_than_blocking(monkeypatch):
    _count_calls(monkeypatch, lambda req: _Resp([{"tag_name": "v1.0.0"}]))
    release_fetch._release_list(log=lambda _m: None)
    cached = json.loads(release_fetch.RELEASES_CACHE.read_text())
    cached["fetched_at"] = 0
    release_fetch.RELEASES_CACHE.write_text(json.dumps(cached))

    headers = {"X-RateLimit-Remaining": "0",
               "X-RateLimit-Limit": "60",
               "X-RateLimit-Reset": str(int(time.time()) + 600)}
    _count_calls(monkeypatch, lambda req: (_ for _ in ()).throw(_http_error(403, headers)))

    messages = []
    assert release_fetch._release_list(log=messages.append) == [{"tag_name": "v1.0.0"}]
    assert "rate limit" in messages[0].lower()
    assert "cached" in messages[0]


def test_rate_limit_error_reports_the_reset_time(monkeypatch):
    reset = int(time.time()) + 1800
    headers = {"X-RateLimit-Remaining": "0",
               "X-RateLimit-Limit": "60",
               "X-RateLimit-Reset": str(reset)}
    _count_calls(monkeypatch, lambda req: (_ for _ in ()).throw(_http_error(403, headers)))

    # No cache to fall back on, so this must surface as a usable error.
    with pytest.raises(ReleaseError) as excinfo:
        release_fetch._release_list(log=lambda _m: None)
    message = str(excinfo.value)
    assert "resets at" in message
    assert time.strftime("%H:%M", time.localtime(reset)) in message
    assert "60 requests/hour" in message


def test_unrelated_http_error_is_not_reported_as_a_rate_limit(monkeypatch):
    headers = {"X-RateLimit-Remaining": "57"}
    _count_calls(monkeypatch, lambda req: (_ for _ in ()).throw(_http_error(500, headers)))
    with pytest.raises(ReleaseError, match="500"):
        release_fetch._release_list(log=lambda _m: None)


def test_empty_listing_never_overwrites_a_good_cache(monkeypatch):
    """The 2026-08-17 incident: HTTP 200 with [] for every repo on github.com.

    /releases/latest and GraphQL stayed correct throughout, so this is not
    "unreachable" and the URLError fallback never fires. Treating it as truth
    reported "no firmware release found" and would have wiped the cache that
    still had the answer.
    """
    _count_calls(monkeypatch, lambda req: _Resp([{"tag_name": "v1.8.1"}]))
    release_fetch._release_list(log=lambda _m: None)
    cached = json.loads(release_fetch.RELEASES_CACHE.read_text())
    cached["fetched_at"] = 0
    release_fetch.RELEASES_CACHE.write_text(json.dumps(cached))

    _count_calls(monkeypatch, lambda req: _Resp([], etag='W/"empty"'))
    messages = []
    assert release_fetch._release_list(log=messages.append) == [{"tag_name": "v1.8.1"}]
    assert "empty release list" in messages[0]
    assert "githubstatus" in messages[0]
    # A zeroed/implausible timestamp must not render as "29782919 min ago".
    assert "previously cached" in messages[0]
    # The good entries survive on disk for the next run too.
    assert json.loads(
        release_fetch.RELEASES_CACHE.read_text())["releases"] == [{"tag_name": "v1.8.1"}]


def test_empty_listing_with_no_cache_blames_the_api_not_the_repo(monkeypatch):
    _count_calls(monkeypatch, lambda req: _Resp([]))
    with pytest.raises(ReleaseError) as excinfo:
        release_fetch._release_list(log=lambda _m: None)
    message = str(excinfo.value)
    assert "empty release list" in message
    assert "API incident" in message


def test_manifests_are_fetched_once_and_reused_forever(monkeypatch):
    """Release assets are immutable, so a manifest URL can never change."""
    calls = []

    def fake_get(url):
        calls.append(url)
        return {"schema_version": 1, "tag": "lua-v1.2.0"}

    monkeypatch.setattr(release_fetch, "_get_json", fake_get)
    url = "https://example.test/lua-v1.2.0/main.lua.manifest.json"
    first = release_fetch._cached_manifest(url)
    second = release_fetch._cached_manifest(url)
    assert first == second
    assert len(calls) == 1

    # And it survives a restart: the store is on disk, not in memory.
    def explode(_url):
        raise AssertionError("must not refetch a cached manifest")

    monkeypatch.setattr(release_fetch, "_get_json", explode)
    assert release_fetch._cached_manifest(url) == first


def test_a_new_manifest_url_is_still_fetched(monkeypatch):
    monkeypatch.setattr(release_fetch, "_get_json", lambda url: {"url": url})
    a = release_fetch._cached_manifest("https://example.test/a.manifest.json")
    b = release_fetch._cached_manifest("https://example.test/b.manifest.json")
    assert a != b

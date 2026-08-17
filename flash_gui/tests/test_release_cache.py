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


@pytest.fixture(autouse=True)
def _isolated_cache(tmp_path, monkeypatch):
    monkeypatch.setattr(release_fetch, "CACHE_DIR", tmp_path)
    monkeypatch.setattr(release_fetch, "RELEASES_CACHE", tmp_path / "releases.json")


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

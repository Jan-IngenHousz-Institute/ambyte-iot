# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Setup requires a deployment decision, independent of the laptop timezone."""
import sys
from types import SimpleNamespace
import pytest

from flash_gui import timezones


@pytest.fixture
def resolve(monkeypatch):
    calls = []
    def zone_for(lat, lon):
        calls.append((lat, lon))
        return 'Australia/Darwin'
    monkeypatch.setitem(sys.modules, 'flash_gui.tz_geo', SimpleNamespace(zone_for=zone_for))
    monkeypatch.setattr(timezones, 'local_iana_zone', lambda: pytest.fail('Laptop zone must not be read'))
    return calls


def test_known_deployment_coordinates_choose_zone_and_survive_setup(resolve):
    assert timezones.setup_zone('-25.3444', '131.0369', '') == ('Australia/Darwin', -25.3444, 131.0369)
    assert resolve == [(-25.3444, 131.0369)]


def test_operator_can_manually_override_the_coordinate_zone(resolve):
    assert timezones.setup_zone('-25.3444', '131.0369', 'Europe/Berlin')[0] == 'Europe/Berlin'


def test_no_coordinates_require_explicit_selection(resolve):
    with pytest.raises(ValueError, match='Choose the fallback'):
        timezones.setup_zone('', '', '')
    assert timezones.setup_zone('', '', 'Europe/Amsterdam') == ('Europe/Amsterdam', None, None)
    assert not resolve


def test_unset_zero_coordinates_require_selection(resolve):
    with pytest.raises(ValueError, match='Choose the fallback'):
        timezones.setup_zone('0', '0', '')
    assert not resolve


def test_map_no_match_requires_manual_zone(monkeypatch, resolve):
    monkeypatch.setitem(sys.modules, 'flash_gui.tz_geo', SimpleNamespace(zone_for=lambda *a: None))
    with pytest.raises(ValueError, match='Choose the fallback'):
        timezones.setup_zone('1', '-30', '')
    assert timezones.setup_zone('1', '-30', 'Etc/UTC') == ('Etc/UTC', 1, -30)


@pytest.mark.parametrize('lat,lon', [('52', ''), ('', '5'), ('nan', '5'), ('52', 'inf'), ('91', '5'), ('52', '-181'), ('a', 'b')])
def test_invalid_coordinates_block_setup_before_writes(resolve, lat, lon):
    with pytest.raises(ValueError, match='Enter both deployment coordinates'):
        timezones.setup_zone(lat, lon, 'Europe/Amsterdam')
    assert not resolve


def test_invalid_manual_zone_is_rejected(resolve):
    with pytest.raises(ValueError, match='not in the firmware'):
        timezones.setup_zone('', '', 'My/Laptop')


def test_provisioning_writes_fallback_without_pinning_the_device():
    from flash_gui.tests.test_flash_gui import make_plan
    from flash_gui.nvs_builder import build_nvs_csv
    assert 'tz_source,' not in build_nvs_csv(make_plan())
    csv=build_nvs_csv(make_plan(timezone='Australia/Darwin',lat=-25.3444,lon=131.0369))
    assert 'tz_source,' not in csv and 'site_state,data,hex2bin,' in csv

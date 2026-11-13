# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Evaluate the firmware's bundled POSIX TZ rules without host tzdata.

Windows has no system zoneinfo database. More importantly, the device runs the
newlib-compatible rules generated here, including the generator's transition
clamps; a newer host IANA database must not substitute different rules during
USB verification. This evaluator supports the same POSIX subset as that table.
It never changes process-global TZ, so GUI workers and the laptop clock remain
independent. An unsupported future rule fails closed as runtime unverified.
"""
import calendar
from datetime import datetime, timedelta, timezone
import re
from .tz_zone_table import POSIX_RULES

_ABBR = r'(?:[A-Za-z]{3,}|<[+-]?[A-Za-z0-9]+>)'
_OFF = r'[+-]?\d{1,3}(?::\d{1,2}(?::\d{1,2})?)?'
_RULE = re.compile(rf'{_ABBR}(?P<std>{_OFF})(?:{_ABBR}(?P<dst>{_OFF})?,(?P<start>[^,]+),(?P<end>[^,]+))?')


def _seconds(text):
    sign = -1 if text.startswith('-') else 1
    parts = [int(part) for part in text.lstrip('+-').split(':')]
    if len(parts) > 3 or any(part > 59 for part in parts[1:]):
        raise ValueError('Unsupported POSIX offset')
    result = sum(part * multiplier for part, multiplier in zip(parts, (3600, 60, 1)))
    if result > 86400:
        raise ValueError('Unsupported POSIX offset')
    return sign * result


def _transition(text, year, previous_offset):
    date, slash, clock = text.partition('/')
    start = datetime(year, 1, 1, tzinfo=timezone.utc)
    if match := re.fullmatch(r'M(\d{1,2})\.([1-5])\.([0-6])', date):
        month, week, weekday = map(int, match.groups())
        first, days = calendar.monthrange(year, month)
        day = 1 + (weekday - (first + 1) % 7) % 7 + (week - 1) * 7
        if day > days:
            day -= 7
        moment = datetime(year, month, day, tzinfo=timezone.utc)
    elif re.fullmatch(r'J\d{1,3}', date):
        day = int(date[1:])
        if not 1 <= day <= 365:
            raise ValueError('Unsupported Julian date')
        moment = start + timedelta(days=day - 1 + (calendar.isleap(year) and day >= 60))
    elif re.fullmatch(r'\d{1,3}', date) and 0 <= int(date) <= 365:
        moment = start + timedelta(days=int(date))
    else:
        raise ValueError('Unsupported POSIX transition')
    return int(moment.timestamp()) + (_seconds(clock) if slash else 7200) - previous_offset


def offset_for(zone, utc_epoch):
    """Seconds local-minus-UTC at the device's UTC, using its compiled rule."""
    rule = POSIX_RULES[zone]
    match = _RULE.fullmatch(rule)
    if match is None:
        raise ValueError('Unsupported firmware timezone rule')
    standard = -_seconds(match['std'])  # POSIX writes UTC-minus-local.
    if match['start'] is None:
        return standard
    summer = -_seconds(match['dst']) if match['dst'] else standard + 3600
    year = datetime.fromtimestamp(utc_epoch, timezone.utc).year
    # The most recent event handles southern seasons, negative DST, /24 and
    # transitions whose UTC year differs from their local calendar year.
    events = []
    for candidate in range(year - 1, year + 2):
        events += [(_transition(match['start'], candidate, standard), summer),
                   (_transition(match['end'], candidate, summer), standard)]
    return max(event for event in events if event[0] <= utc_epoch)[1]

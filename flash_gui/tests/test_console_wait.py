# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The post-flash console wait must diagnose, not just time out.

Field case (2026-08-25): a collaborator's PC never saw `ambyte> ` after a
perfect flash, twice in a row, and the pre-flash probe on an already-provisioned
board was silent too. The old loop held a dark port for 180 s and said "the
board may still be booting". These tests pin the behaviours that turn that into
a verdict: silence → ROM-downloader check + reset; repeated reset banners → boot
loop with reason; unopenable port → named in the report.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from flash_gui import ambyte_serial                             # noqa: E402
from flash_gui.ambyte_serial import AmbyteConsole, ConsoleError  # noqa: E402


class FakeConsole:
    """Scripted stand-in for AmbyteConsole; each instance follows `plan`."""

    instances: list["FakeConsole"] = []
    plan: list[dict] = []          # one dict per successive open()

    def __init__(self, port):
        spec = self.plan.pop(0) if self.plan else {}
        if spec.get("open_error"):
            raise serial.SerialException(spec["open_error"])
        self.port = port
        self.closed = False
        self.reset_pulsed = False
        self._answers = list(spec.get("answers", [False]))
        self.rx_total = spec.get("rx_total", 0)
        self._resets = spec.get("resets", 0)
        self._reason = spec.get("reason")
        self._panic = spec.get("panic")
        self._tail = spec.get("tail", "")
        FakeConsole.instances.append(self)

    def wait_prompt(self, timeout):
        if len(self._answers) > 1:
            return self._answers.pop(0)
        return self._answers[0]

    def close(self):
        self.closed = True

    def reset_count(self):
        return self._resets

    def last_reset_reason(self):
        return self._reason

    def last_panic(self):
        return self._panic

    def rx_tail(self, lines=6):
        return self._tail

    def pulse_reset(self):
        self.reset_pulsed = True


@pytest.fixture
def fast_wait(monkeypatch):
    """No real sleeping, thresholds at zero, one visible port."""
    FakeConsole.instances = []
    FakeConsole.plan = []
    monkeypatch.setattr(ambyte_serial, "AmbyteConsole", FakeConsole)
    monkeypatch.setattr(ambyte_serial, "esp_jtag_ports",
                        lambda exclude=None: ["COM7"])
    monkeypatch.setattr(ambyte_serial.time, "sleep", lambda _s: None)
    monkeypatch.setattr(ambyte_serial, "CONSOLE_SILENCE_RESET_S", 0.0)
    monkeypatch.setattr(ambyte_serial, "BOOT_LOOP_RESETS", 4)
    logs: list[str] = []
    return logs


def test_silent_board_is_checked_for_download_mode_and_reset(fast_wait,
                                                             monkeypatch):
    """Zero bytes → ask the ROM; when it answers, keep waiting on a fresh open."""
    logs = fast_wait
    asked = []
    monkeypatch.setattr(ambyte_serial.esptool_ops, "rom_bootloader_answers",
                        lambda port: asked.append(port) or True)
    FakeConsole.plan = [
        {"answers": [False]},              # dark port, no bytes
        {"answers": [True], "rx_total": 200},   # after the reset: prompt
    ]

    con = ambyte_serial.connect_after_boot(
        "COM7", deadline_s=30.0, log=logs.append, settle_s=0.0)

    assert asked == ["COM7"]
    assert con is FakeConsole.instances[1]
    assert FakeConsole.instances[0].closed
    assert any("ROM downloader answered" in m for m in logs)


def test_silent_board_gets_one_reset_pulse_when_rom_is_quiet(fast_wait,
                                                             monkeypatch):
    logs = fast_wait
    monkeypatch.setattr(ambyte_serial.esptool_ops, "rom_bootloader_answers",
                        lambda port: False)
    FakeConsole.plan = [
        {"answers": [False]},              # dark
        {},                                # the handle used for pulse_reset
        {"answers": [True], "rx_total": 10},
    ]

    con = ambyte_serial.connect_after_boot(
        "COM7", deadline_s=30.0, log=logs.append, settle_s=0.0)

    assert FakeConsole.instances[1].reset_pulsed
    assert con is FakeConsole.instances[2]
    assert any("pulsing the reset line" in m for m in logs)


def test_boot_loop_fails_early_with_reason(fast_wait, monkeypatch):
    logs = fast_wait
    monkeypatch.setattr(ambyte_serial.esptool_ops, "rom_bootloader_answers",
                        lambda port: pytest.fail("ROM check must not run: bytes were seen"))
    FakeConsole.plan = [{
        "answers": [False], "rx_total": 4000, "resets": 5,
        "reason": "RTCWDT_RTC_RESET",
        "panic": "Brownout detector was triggered",
        "tail": "rst:0x10 (RTCWDT_RTC_RESET),boot:0x8",
    }]

    with pytest.raises(ConsoleError) as exc:
        ambyte_serial.connect_after_boot(
            "COM7", deadline_s=30.0, log=logs.append, settle_s=0.0)

    msg = str(exc.value)
    assert "boot-looping" in msg
    assert "RTCWDT_RTC_RESET" in msg
    assert "Brownout" in msg
    assert "rst:0x10" in msg
    assert FakeConsole.instances[0].closed


def test_unopenable_port_is_named_once_and_in_the_verdict(fast_wait,
                                                          monkeypatch):
    """A port another program holds must not look like a slow board."""
    logs = fast_wait
    monkeypatch.setattr(ambyte_serial.esptool_ops, "rom_bootloader_answers",
                        lambda port: False)
    # Every open fails; run until the (fake) clock passes the deadline.
    FakeConsole.plan = []
    class AlwaysBusy(FakeConsole):
        def __init__(self, port):
            raise serial.SerialException("PermissionError(13, 'Access is denied.')")
    monkeypatch.setattr(ambyte_serial, "AmbyteConsole", AlwaysBusy)
    clock = {"t": 1000.0}
    def fake_time():
        clock["t"] += 5.0
        return clock["t"]
    monkeypatch.setattr(ambyte_serial.time, "time", fake_time)

    with pytest.raises(ConsoleError) as exc:
        ambyte_serial.connect_after_boot(
            "COM7", deadline_s=60.0, log=logs.append, settle_s=0.0)

    msg = str(exc.value)
    assert "printed NOTHING" in msg
    assert "would not open: COM7 (PermissionError" in msg
    assert sum("Cannot open COM7" in m for m in logs) == 1
    assert any(m.startswith("Still waiting") for m in logs)


def test_console_captures_tail_and_reset_banners():
    class FakeSerial:
        def __init__(self, chunks):
            self._chunks = list(chunks)
        def read(self, n):
            return self._chunks.pop(0) if self._chunks else b""

    con = object.__new__(AmbyteConsole)
    con.port = "COM7"
    con.rx_total = 0
    con._rx_tail = ""
    con._ser = FakeSerial([
        b"ESP-ROM:esp32s3\r\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n",
        b"\x1b[0;32mI (300) cpu_start: Pro cpu up.\x1b[0m\r\n",
        b"Guru Meditation Error: Core 0 panic'ed (LoadProhibited)\r\n",
        b"rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n",
    ])
    for _ in range(4):
        con._read_available()

    assert con.rx_total > 0
    assert con.reset_count() == 2
    assert con.last_reset_reason() == "RTC_SW_SYS_RST"
    assert con.last_panic().startswith("Guru Meditation Error")
    tail = con.rx_tail(2)
    assert "\x1b" not in tail
    assert tail.splitlines()[-1].startswith("rst:0x3")

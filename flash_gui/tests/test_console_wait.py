# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""The post-flash console wait must diagnose, not just time out.

Field case (2026-08-25): on two Windows PCs the board never showed `ambyte> `
after a perfect flash. Hardware-verified cause: esptool's RTS hard reset boots
the S3 into the ROM downloader on this usbser stack (emulated boot strap reads
DTR as asserted) even with GPIO0 high and FORCE_DOWNLOAD_BOOT clear; a
watchdog reset boots the app. These tests pin the behaviours that turn the old
generic timeout into a fix or a verdict: downloader check on a fresh handle →
watchdog reboot; repeated reset banners → boot loop with reason; unopenable
port → named in the report.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from flash_gui import ambyte_serial, esptool_ops                # noqa: E402
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
        self.rescues = 0
        # Per-call rescue outcomes; the last one repeats.
        self._rescue = list(spec.get("rescue", [False]))
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

    def rescue_download_mode(self):
        self.rescues += 1
        if len(self._rescue) > 1:
            return self._rescue.pop(0)
        return self._rescue[0]


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


def test_parked_board_is_rescued_through_the_kept_handle(fast_wait):
    """ROM answers on the first open → clear + reset, same handle, no reopen."""
    logs = fast_wait
    FakeConsole.plan = [{"answers": [False, False, True], "rx_total": 300,
                         "rescue": [True]}]

    con = ambyte_serial.connect_after_boot(
        "COM7", deadline_s=30.0, log=logs.append, settle_s=0.0)

    assert con is FakeConsole.instances[0]
    assert len(FakeConsole.instances) == 1          # never reopened
    assert con.rescues == 1
    assert not con.closed
    assert any("sitting in the ROM downloader" in m for m in logs)


def test_healthy_board_is_checked_once_and_not_reset(fast_wait):
    logs = fast_wait
    FakeConsole.plan = [{"answers": [False, True], "rx_total": 900,
                         "rescue": [False]}]

    con = ambyte_serial.connect_after_boot(
        "COM7", deadline_s=30.0, log=logs.append, settle_s=0.0)

    assert con is FakeConsole.instances[0]
    assert con.rescues == 1
    assert any("not in the ROM downloader" in m for m in logs)


def test_silence_after_a_missed_rescue_retries_on_the_same_handle(fast_wait):
    """First check says 'running', nothing arrives → ask again, same handle."""
    logs = fast_wait
    FakeConsole.plan = [{"answers": [False, False, True], "rx_total": 0,
                         "rescue": [False, True]}]

    con = ambyte_serial.connect_after_boot(
        "COM7", deadline_s=30.0, log=logs.append, settle_s=0.0)

    assert con is FakeConsole.instances[0]
    assert len(FakeConsole.instances) == 1
    assert con.rescues == 2
    assert any("Checking again" in m for m in logs)
    assert any("It was: rebooted it via watchdog" in m for m in logs)


def test_boot_loop_fails_early_with_reason(fast_wait):
    logs = fast_wait
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


def test_rescue_uses_the_given_handle_and_restores_its_settings(monkeypatch):
    """esptool must talk through OUR open handle (never open/close a port),
    clear the sticky bit via the S3 hard_reset, and hand the handle back."""
    events = []

    class FakeSer:
        baudrate = 115200
        timeout = 0.2
        write_timeout = None
        is_open = True
        reopened = 0

        def open(self):
            self.is_open = True
            self.reopened += 1

    class FakeEsp:
        pass

    def fake_detect(port, baud, connect_mode, connect_attempts):
        assert port is ser                     # the very object, not a name
        assert connect_mode == "no-reset"
        events.append("detect")
        port.timeout = 3.0                     # esptool fiddles with these
        port.baudrate = 921600
        return FakeEsp()

    def fake_reset(esp, mode):
        assert isinstance(esp, FakeEsp)
        events.append(mode)

    ser = FakeSer()
    monkeypatch.setattr(esptool_ops, "detect_chip", fake_detect)
    monkeypatch.setattr(esptool_ops, "reset_chip", fake_reset)

    assert esptool_ops.rescue_from_download_mode(ser) is True
    # Watchdog, never RTS: on this Windows stack an RTS reset boots the S3
    # into the downloader (hardware-verified 2026-08-25).
    assert events == ["detect", "watchdog-reset"]
    assert (ser.baudrate, ser.timeout) == (115200, 0.2)

    def no_rom(port, baud, connect_mode, connect_attempts):
        port.is_open = False           # ESPLoader.connect closes it on failure
        raise RuntimeError("Failed to connect")
    monkeypatch.setattr(esptool_ops, "detect_chip", no_rom)
    assert esptool_ops.rescue_from_download_mode(ser) is False
    # The healthy-board path must hand the handle back OPEN (field 2026-08-25:
    # PortNotOpenError in the Lua step's console wait).
    assert ser.is_open and ser.reopened == 1

# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Parser + log-writer tests for the factory PCBA test runner.

The transcript fixtures replicate the firmware's real wire contract
(components/CLI/CLI.c cli_cmd_selftest), including the console echo line and
interleaved ESP_LOG lines from other tasks — the exact noise the anchored
regexes exist to survive.
"""

import csv
import json

import pytest

from flash_gui.factory_test import (FactoryTestError, overall_verdict,
                                    parse_selftest, resolve_images, write_logs)

PASS_REPLY = """selftest
SELFTEST BEGIN fw=1.0.6 mac=E8:F6:0A:B1:1F:34
TEST identity PASS mac=E8:F6:0A:B1:1F:34 chip_rev=2 cores=2 flash_mb=16 t=3ms
TEST psram PASS size=2094080 rw=ok t=41ms
I (52341) wifi_manager: scan done
TEST evstore PASS mounted=1 total=9830400 used=16384 rw=ok t=88ms
TEST littlefs PASS mounted=1 total=524288 used=24576 rw=ok t=61ms
TEST nvs PASS used=42 free=580 t=1ms
TEST i2c PASS bus=ok mp2731=1 rtc=1 bme280=1 extra=0 t=310ms
TEST rtc PASS ready=1 osf=1 tick=ok epoch=1788672000 t=1104ms
TEST power PASS read=ok vbat_mv=3812 vin_mv=5060 vsys_mv=4021 vbus=1 t=12ms
TEST bme280 PASS read=ok temp_c=24.61 hum_pct=41.2 pres_pa=101325 t=9ms
TEST led PASS state=on t=1ms
SELFTEST PASS passed=10 failed=0 fw=1.0.6 mac=E8:F6:0A:B1:1F:34
ambyte> """

FAIL_REPLY = """selftest
SELFTEST BEGIN fw=1.0.6 mac=E8:F6:0A:B1:1F:34
TEST identity PASS mac=E8:F6:0A:B1:1F:34 chip_rev=2 cores=2 flash_mb=16 t=3ms
TEST psram FAIL size=0 rw=fail t=2ms
TEST evstore PASS mounted=1 total=9830400 used=16384 rw=ok t=88ms
TEST littlefs PASS mounted=1 total=524288 used=24576 rw=ok t=61ms
TEST nvs PASS used=42 free=580 t=1ms
TEST i2c FAIL bus=ok mp2731=0 rtc=1 bme280=1 extra=0 t=310ms
TEST rtc PASS ready=1 osf=0 tick=ok epoch=1788672000 t=1104ms
TEST power FAIL read=fail vbat_mv=0 vin_mv=0 vsys_mv=0 vbus=0 t=3ms
TEST bme280 PASS read=ok temp_c=24.61 hum_pct=41.2 pres_pa=101325 t=9ms
TEST led PASS state=on t=1ms
SELFTEST FAIL passed=7 failed=3 fw=1.0.6 mac=E8:F6:0A:B1:1F:34
Command returned non-zero error code: 0x1 (ERROR)
ambyte> """


def test_parse_pass_reply():
    result = parse_selftest(PASS_REPLY)
    assert result.overall_pass
    assert result.mac == "E8:F6:0A:B1:1F:34"
    assert result.fw == "1.0.6"
    assert result.passed == 10 and result.failed == 0
    assert len(result.tests) == 10
    assert result.failed_names == []
    # Raw values survive parsing — the archive depends on them.
    psram = next(t for t in result.tests if t.name == "psram")
    assert psram.values["size"] == "2094080"
    rtc = next(t for t in result.tests if t.name == "rtc")
    # OSF=1 on a factory-fresh board is reported, not failed.
    assert rtc.passed and rtc.values["osf"] == "1"


def test_parse_fail_reply_collects_failed_names():
    result = parse_selftest(FAIL_REPLY)
    assert not result.overall_pass
    assert result.failed == 3
    assert result.failed_names == ["psram", "i2c", "power"]
    # The REPL's non-zero-exit marker must not break parsing.
    assert result.passed == 7


def test_parse_old_firmware_raises():
    with pytest.raises(FactoryTestError, match="no `selftest` command"):
        parse_selftest("selftest\nUnrecognized command\nambyte> ")


def test_parse_missing_verdict_raises():
    with pytest.raises(FactoryTestError, match="no SELFTEST verdict"):
        parse_selftest("selftest\nTEST psram PASS size=1 rw=ok t=1ms\nambyte> ")


def test_write_logs_json_and_csv(tmp_path):
    result = parse_selftest(PASS_REPLY)
    json_path = write_logs(tmp_path, result, port="COM7", operator="lc",
                           station="bench-1", led_operator=True, overall=True,
                           duration_s=12.3)
    record = json.loads(json_path.read_text(encoding="utf-8"))
    assert record["mac"] == "E8:F6:0A:B1:1F:34"
    assert record["overall"] == "PASS"
    assert record["led_operator_pass"] is True
    assert record["transcript"] == PASS_REPLY
    assert len(record["tests"]) == 10

    rows = list(csv.DictReader((tmp_path / "results.csv").open(encoding="utf-8")))
    assert len(rows) == 1
    assert rows[0]["overall"] == "PASS"
    assert rows[0]["json_file"] == json_path.name


def test_write_logs_never_overwrites(tmp_path):
    result = parse_selftest(FAIL_REPLY)
    first = write_logs(tmp_path, result, port="COM7", operator="", station="",
                       led_operator=False, overall=False, duration_s=1.0)
    second = write_logs(tmp_path, result, port="COM7", operator="", station="",
                        led_operator=None, overall=False, duration_s=1.0)
    assert first != second and first.exists() and second.exists()

    rows = list(csv.DictReader((tmp_path / "results.csv").open(encoding="utf-8")))
    assert len(rows) == 2
    assert rows[0]["led_operator"] == "no"
    assert rows[1]["led_operator"] == "skipped"
    assert rows[0]["failed_tests"] == "psram;i2c;power"


def test_resolve_images_local_build_dir(tmp_path):
    """A PIO/IDF build dir with flasher_args.json is flashable as-is; the
    manifest's flash_files (which never include NVS) drive the write list."""
    (tmp_path / "bootloader").mkdir()
    (tmp_path / "bootloader" / "bootloader.bin").write_bytes(b"\xe9boot")
    (tmp_path / "app.bin").write_bytes(b"\xe9app")
    (tmp_path / "flasher_args.json").write_text(json.dumps({
        "flash_settings": {"flash_mode": "dio", "flash_size": "16MB",
                           "flash_freq": "80m"},
        "flash_files": {"0x0": "bootloader/bootloader.bin",
                        "0x20000": "app.bin"},
    }), encoding="utf-8")

    images = resolve_images(str(tmp_path), log=lambda _msg: None)
    assert images.tag == "local"
    offsets = [off for off, _ in images.flash_files]
    assert offsets == [0x0, 0x20000]
    assert images.flash_settings["flash_size"] == "16MB"


def test_resolve_images_pio_flattened_layout(tmp_path):
    """PlatformIO flattens the IDF layout but keeps IDF's manifest paths; the
    resolver must re-point every entry at the flattened file names."""
    for name in ("bootloader.bin", "partitions.bin", "firmware.bin",
                 "ota_data_initial.bin"):
        (tmp_path / name).write_bytes(b"\xe9x")
    (tmp_path / "flasher_args.json").write_text(json.dumps({
        "flash_settings": {"flash_mode": "dio", "flash_size": "16MB",
                           "flash_freq": "80m"},
        "flash_files": {"0x0": "bootloader/bootloader.bin",
                        "0x8000": "partition_table/partition-table.bin",
                        "0xf000": "ota_data_initial.bin",
                        "0x20000": "ambyte-iot.bin"},
        "app": {"offset": "0x20000", "file": "ambyte-iot.bin"},
    }), encoding="utf-8")

    images = resolve_images(str(tmp_path), log=lambda _msg: None)
    resolved = {off: p.name for off, p in images.flash_files}
    assert resolved == {0x0: "bootloader.bin", 0x8000: "partitions.bin",
                        0xF000: "ota_data_initial.bin", 0x20000: "firmware.bin"}


def test_resolve_images_rejects_bad_path(tmp_path):
    with pytest.raises(FactoryTestError, match="flasher_args.json"):
        resolve_images(str(tmp_path / "nope"), log=lambda _msg: None)


def test_write_logs_records_flashed_tag(tmp_path):
    result = parse_selftest(PASS_REPLY)
    json_path = write_logs(tmp_path, result, port="COM7", operator="",
                           station="", led_operator=None, overall=True,
                           duration_s=60.0, flashed="v1.0.7")
    record = json.loads(json_path.read_text(encoding="utf-8"))
    assert record["flashed"] == "v1.0.7"


def test_overall_verdict_rules():
    """A firmware-PASS board with a dead LED is an overall FAIL; a skipped
    prompt (--no-led) leaves the firmware verdict untouched."""
    assert overall_verdict(True, True) is True
    assert overall_verdict(True, False) is False
    assert overall_verdict(True, None) is True
    assert overall_verdict(False, True) is False
    assert overall_verdict(False, None) is False

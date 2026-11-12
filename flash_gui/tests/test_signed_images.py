"""Flashing rejects unbootable modern intermediates before opening a port."""
from pathlib import Path
import sys
from unittest.mock import Mock

import pytest
from cryptography.hazmat.primitives.serialization import load_pem_private_key
from flash_gui import esptool_ops
from flash_gui.firmware_metadata import sign_image

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tests"))
from ota_fixture import image


@pytest.fixture(scope="module")
def key():
    return load_pem_private_key((ROOT/"tools/keys/UNSAFE_BENCH_ONLY_firmware.pem").read_bytes(),None)


def test_modern_unsigned_local_or_factory_input_never_opens_serial(tmp_path,monkeypatch):
    app=tmp_path/"firmware.bin";app.write_bytes(image())
    write=Mock();monkeypatch.setattr(esptool_ops,"_write",write)
    with pytest.raises(esptool_ops.EsptoolError,match="modern image is unsigned"):
        esptool_ops.flash_images("bench",[(0x10000,app)],{})
    write.assert_not_called()


def test_signature_presence_and_storage_versions_display_before_flash(tmp_path,monkeypatch,key):
    app=tmp_path/"firmware.bin";app.write_bytes(sign_image(image(),key))
    write=Mock();monkeypatch.setattr(esptool_ops,"_write",write)
    log=[];esptool_ops.flash_images("bench",[(0x10000,app)],{},log.append)
    write.assert_called_once()
    assert any("present (embedded-key consistency checked)" in line for line in log)
    assert any("readable [1], written 1, fallback 1" in line for line in log)
    assert all("release-trusted" not in line for line in log)


def test_legacy_unsigned_supported_with_unavailable_manifest(tmp_path,monkeypatch):
    app=tmp_path/"firmware.bin";app.write_bytes(image(modern=False))
    write=Mock();monkeypatch.setattr(esptool_ops,"_write",write)
    log=[];esptool_ops.flash_images("bench",[(0x10000,app)],{},log.append)
    write.assert_called_once()
    assert any("signature: absent" in line for line in log)
    assert any("unavailable on legacy firmware" in line for line in log)


@pytest.mark.parametrize("offset",[0x120,0x128,0x160,4096+1196])
def test_corrupt_signed_input_never_opens_port(tmp_path,monkeypatch,key,offset):
    data=bytearray(sign_image(image(),key));data[offset]^=1
    app=tmp_path/"firmware.bin";app.write_bytes(data)
    write=Mock();monkeypatch.setattr(esptool_ops,"_write",write)
    with pytest.raises(esptool_ops.EsptoolError):esptool_ops.flash_images("bench",[(0x10000,app)],{})
    write.assert_not_called()

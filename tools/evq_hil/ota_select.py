"""Render the one otadata sector that selects an OTA slot exactly as
esp_ota_set_boot_partition does with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE:
ota_seq = next sequence for the slot, ota_state = ESP_OTA_IMG_NEW, crc =
crc32(seq, 0xFFFFFFFF), written to the sector NOT holding the newest valid
entry (contract W-1(ii), P0-4). ESP-IDF's otatool.py computes the same seq but
keeps the old sector's ota_state bytes, so it is not used for the write.

    python ota_select.py --otadata cur.bin --slot 0 --out sector.bin
    -> prints {"offset": "0xf000"|"0x10000", "seq": N, ...}; flash with
       esptool write_flash <offset> sector.bin (4 KiB)
"""
from __future__ import annotations

import argparse
import binascii
import json
import struct
from pathlib import Path

SECTOR = 0x1000
OTADATA_OFFSET = 0xF000
ESP_OTA_IMG_NEW = 0x0
STATES = {0x0: "NEW", 0x1: "PENDING_VERIFY", 0x2: "VALID", 0x3: "INVALID", 0x4: "ABORTED", 0xFFFFFFFF: "UNDEFINED"}


def parse(otadata: bytes) -> list[dict]:
    out = []
    for i in range(2):
        e = otadata[i * SECTOR:i * SECTOR + 32]
        seq, = struct.unpack_from("<I", e, 0)
        state, crc = struct.unpack_from("<II", e, 24)
        valid = seq != 0xFFFFFFFF and crc == binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF)
        out.append({"sector": i, "offset": hex(OTADATA_OFFSET + i * SECTOR), "seq": seq, "state": state,
                    "state_name": STATES.get(state, hex(state)), "crc": crc, "valid": valid,
                    "slot": ((seq - 1) % 2) if valid else None})
    return out


def select(otadata: bytes, slot: int, n_slots: int = 2) -> tuple[int, bytes, dict]:
    info = parse(otadata)
    valid = [e for e in info if e["valid"]]
    base = max(valid, key=lambda e: e["seq"]) if valid else None
    target = slot + 1
    if base is None:
        seq = target
        write_sector = 0
    else:
        i = 0
        while base["seq"] > target % n_slots + i * n_slots:
            i += 1
        seq = target % n_slots + i * n_slots
        if seq == base["seq"]:
            raise ValueError(f"slot {slot} is already selected (seq {seq})")
        write_sector = 1 - base["sector"]
    entry = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<I", ESP_OTA_IMG_NEW) + \
        struct.pack("<I", binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF))
    sector = entry + b"\xff" * (SECTOR - len(entry))
    return OTADATA_OFFSET + write_sector * SECTOR, sector, {"base": base, "seq": seq, "slot": slot,
                                                           "state": "NEW", "write_sector": write_sector}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--otadata", required=True, help="current 8 KiB otadata region (read_flash 0xf000 0x2000)")
    ap.add_argument("--slot", type=int, choices=[0, 1])
    ap.add_argument("--out")
    ap.add_argument("--parse-only", action="store_true")
    a = ap.parse_args()
    data = Path(a.otadata).read_bytes()
    if len(data) != 2 * SECTOR:
        raise SystemExit(f"otadata must be {2 * SECTOR} bytes, got {len(data)}")
    if a.parse_only:
        print(json.dumps(parse(data)))
        return 0
    off, sector, meta = select(data, a.slot)
    if a.out:
        Path(a.out).write_bytes(sector)
    print(json.dumps({"offset": hex(off), **meta}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

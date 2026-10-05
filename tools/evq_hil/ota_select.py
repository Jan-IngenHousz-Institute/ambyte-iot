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
        # `selectable` is ESP-IDF's bootloader_common_ota_select_valid: an entry the
        # bootloader rolled back (INVALID) or found unconfirmed (PENDING_VERIFY ->
        # ABORTED) keeps a good CRC but is never booted and never the base of a new
        # selection. Treating it as the base would overwrite the sector holding the
        # rollback target (the R entry) - the Sprint 2 replacement-amendment case.
        selectable = valid and state not in (0x3, 0x4)
        out.append({"sector": i, "offset": hex(OTADATA_OFFSET + i * SECTOR), "seq": seq, "state": state,
                    "state_name": STATES.get(state, hex(state)), "crc": crc, "valid": valid,
                    "selectable": selectable, "slot": ((seq - 1) % 2) if valid else None})
    return out


def active(info: list[dict]) -> dict | None:
    """bootloader_common_get_active_otadata on the RAW bytes: the highest-seq
    selectable entry, or None (both unusable). This is the base of a new
    selection; it is NOT what boots next when an entry is PENDING_VERIFY."""
    sel = [e for e in info if e["selectable"]]
    return max(sel, key=lambda e: e["seq"]) if sel else None


def next_boot(info: list[dict]) -> dict | None:
    """What the rollback-enabled bootloader boots on the next reset: its pre-pass
    first turns every PENDING_VERIFY entry into ABORTED (bootloader_utility.c,
    before selection), then boots the active entry. None = no usable entry (the
    no-factory path would then try ota_0 unprotected). An exit is safe only if
    this is VALID, or NEW for a planned candidate exit - never PENDING_VERIFY."""
    after = [dict(e, state=0x4, state_name="ABORTED", selectable=False) if e["valid"] and e["state"] == 0x1
             else e for e in info]
    return active(after)


def select(otadata: bytes, slot: int, n_slots: int = 2) -> tuple[int, bytes, dict]:
    info = parse(otadata)
    base = active(info)                  # esp_rewrite_ota_data's active_otadata
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
        info = parse(data)
        act, nb = active(info), next_boot(info)
        print(json.dumps({"entries": info, "active_sector": act["sector"] if act else None,
                          "active_slot": act["slot"] if act else None,
                          "active_state": act["state_name"] if act else None,
                          "next_boot_slot": nb["slot"] if nb else None,
                          "next_boot_state": nb["state_name"] if nb else None}))
        return 0
    off, sector, meta = select(data, a.slot)
    if a.out:
        Path(a.out).write_bytes(sector)
    print(json.dumps({"offset": hex(off), **meta}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

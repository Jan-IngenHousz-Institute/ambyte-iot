# ambyte flash bundle — flash without compiling

A self-contained bundle that flashes a fully-provisioned ambyte firmware onto a
board **without building anything**. Hand this whole `flash/` folder to whoever
does the flashing (zip it, copy to a USB stick, etc.) and they run one script.

> ⚠️ **Contains secrets.** `bin/nvs.bin` and `reference/` embed the Wi-Fi
> password, the AWS IoT endpoint, and the device **private key**. Transfer the
> folder privately. It is git-ignored on purpose (see `.gitignore`) — do not
> commit or push `bin/`, `reference/`, or the certs/`.env`.

## Flash a board

1. Connect the board by its **USB-C** port (the native USB-Serial-JTAG).
2. Run the script for your OS from inside this folder:

   **Windows**
   ```
   flash.cmd
   ```
   **macOS / Linux**
   ```
   ./flash.sh
   ```

   Pass the serial port if auto-detect picks the wrong one, and see all options
   with `--help`:
   ```
   flash.cmd --port COM7
   ./flash.sh --port /dev/tty.usbmodem2101
   ```

3. When it finishes, the board reboots into the new firmware, connects to Wi-Fi,
   and comes up on MQTT with a per-board identity (`AMBYTE_<MAC>` — the `{MAC}`
   token is expanded on-device, so this one bundle works for **every** board).

### Allow-list gate

Before writing anything, the flasher reads the connected board's base MAC
(`esptool read_mac`) and **refuses to flash unless that MAC is listed in
`allowed_macs.txt`** — a guard against flashing the wrong hardware. It is *not* a
certificate check: whether a board is authorised on AWS IoT is enforced by the
policy attached to the shared device cert, not by flashing.

```
flash.cmd --list      # show the allow-listed MACs
flash.cmd --any       # bypass the gate and flash whatever is connected
flash.cmd --dry-run   # read MAC + check the gate + preview, write nothing
flash.cmd --yes       # skip the y/N confirmation
```

To permit a new board, add its base MAC (one per line) to `allowed_macs.txt`.

No PlatformIO project, no ESP-IDF, and no compiler are needed. The scripts prefer
the esptool that already ships inside a PlatformIO install (zero extra installs);
if PlatformIO isn't present they fall back to `python -m esptool`, installing
`esptool` on first use.

## What's inside

```
flash/
  flash.cmd        Windows launcher  -> flash.py
  flash.sh         macOS/Linux launcher -> flash.py
  flash.py         the flasher: read MAC, gate on allow-list, esptool write
  allowed_macs.txt base MACs this bundle may flash (edit to add boards)
  README.md        this file
  bin/             the compiled images that get written to flash:
    bootloader.bin        -> 0x0
    partitions.bin        -> 0x8000
    nvs.bin               -> 0x9000   (provisioning: certs + Wi-Fi/MQTT)
    ota_data_initial.bin  -> 0xf000
    firmware.bin          -> 0x20000  (the application)
  reference/       NOT needed to flash — kept for auditing / regeneration:
    .env                  the provisioning values baked into nvs.bin
    device_certs/<id>/    the certificate bundle baked into nvs.bin
```

Chip `esp32s3`, flash `dio / 16MB / 80m`. The offsets and flash settings mirror
the build's `flasher_args.json` plus `nvs @ 0x9000` (from `partitions.csv`).

The flasher writes only these regions — it does **not** erase the whole chip, so
a re-flash preserves field data in `coredump` / `littlefs` / `storage`.

## Regenerating the bundle (maintainers)

`bin/` is a snapshot of one firmware build and goes stale after the next build.
To refresh it from the repo root after `pio run`:

```sh
BD=.pio/build/esp32-s3-devkitm-1
cp "$BD"/{bootloader,partitions,ota_data_initial,firmware}.bin flash/bin/
python tools/build_nvs_image.py --out flash/bin/nvs.bin --quiet
cp .env flash/reference/.env
cp -r "device_certs/$(grep -E '^AMBYTE_CERT_BUNDLE=' .env | cut -d= -f2)" flash/reference/device_certs/
```

`nvs.bin` is rebuilt from the current `.env` + cert bundle, so it always matches
what's in `reference/`.

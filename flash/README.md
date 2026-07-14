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

3. It asks for a **device name** (the MQTT payload `device_name` field). Press
   Enter to keep the default `AMBYTE_<MAC>`, or type a friendly name (e.g.
   `Roof-3`). A custom name is applied to the board over the USB console right
   after flashing — see [Device name](#device-name) below.

4. When it finishes, the board reboots into the new firmware, connects to Wi-Fi,
   and comes up on MQTT with a per-board identity (`AMBYTE_<MAC>` — the `{MAC}`
   token is expanded on-device, so this one bundle works for **every** board).

### Device name

The `device_name` in the MQTT payload defaults to `AMBYTE_<MAC>` (baked into
`bin/nvs.bin`; the firmware expands `{MAC}` at boot). To give a board a
human-friendly name instead, type it at the prompt (or pass `--name`):

```
flash.cmd --name Roof-3      # set device_name, skip the prompt
flash.cmd --yes              # non-interactive: keep the default AMBYTE_<MAC>
```

A custom name is **not** baked into `nvs.bin` (that would need a rebuild, which
breaks the compile-free bundle). Instead, after flashing, the script reconnects
to the board's USB-Serial-JTAG console and runs `cfg set device_name <name>`
followed by `reboot`. If that step can't reach the console it just prints the
one command to run by hand — the board is already flashed and works meanwhile.

The MQTT **client-id** and **topic-root** are never affected; they always stay
`AMBYTE_<MAC>`.

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

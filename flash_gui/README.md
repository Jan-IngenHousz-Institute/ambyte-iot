# ambyte on-boarding GUI

Cross-platform (Windows / Linux / macOS) Tkinter tool that flashes and
provisions ambyte boards in one click per device. It replaces the manual
`flash/flash.cmd` bundle flow for operator on-boarding sessions.

```
python -m pip install -r flash_gui/requirements.txt
python -m flash_gui        # from the repo root
```

## Prebuilt executables

CI (`.github/workflows/flash-gui-build.yml`) builds standalone executables for
Windows, Linux and macOS with PyInstaller — no Python install needed on the
operator's machine:

- every PR / main push touching `flash_gui/**` publishes them as workflow
  artifacts (30-day retention, GitHub login required);
- pushing a tag `flash-gui-v*` (e.g. `flash-gui-v0.1.0`) attaches them to a
  public GitHub release under that tag. The flash GUI belongs to **no**
  semantic-release unit — `flash_gui/**` commits never bump the firmware
  version (`tools/release/path-scoped.js`), so these tags are cut by hand.

The binaries are unsigned: expect a SmartScreen "unrecognized app" prompt on
Windows and a Gatekeeper right-click-Open dance on macOS.

Release assets are the raw platform executables (`*-macos`, `*-linux`, and
`*-windows.exe`), not zip or tar archives. A browser download may strip the
executable bit on macOS/Linux; restore it with `chmod +x <downloaded-file>`.

The executables bundle Mozilla's CA root store through `certifi`; HTTPS does
not depend on Python's build-machine certificate paths existing on the
operator's computer. Platform roots are retained as well, so managed machines
can continue to trust locally installed proxy certificates.

## Operator flow (10 boards in a row)

Once per session:

1. Pick the **environment** (dev / prod).
2. Click **Sign in (API key)** — a browser opens on the openJII *API keys*
   page; create a key there (shown once, `jii_...`) and paste it into the
   dialog. The key is validated and remembered per environment.
3. Pick the **experiment** from the dropdown (active experiments you are a
   member of). The **MQTT topic root** is pre-filled from it and stays
   editable; `{thingName}` is replaced per board.
4. Enter the **Wi-Fi SSID/password** the boards should join.

Per board: plug it in via USB-C → **Refresh** → pick the port → **On-board
device** → confirm/edit the name in the prompt. Everything else is automatic,
and the procedure ends with an explicit per-item PASS/FAIL (name, timezone,
RTC).

## What one procedure does

1. **Pre-flash check** — 2 s console probe (one retry) for a running ambyte
   firmware and its stored name; falls back to reading the MAC with esptool
   (works on unflashed chips). The name prompt is pre-filled with the stored
   name, else `AMBYTE_<MAC>`.
2. **openJII registration + certificate** — registers the MAC as an `ambyte`
   device (or re-uses the existing registration), issues — or rotates, when a
   live cert exists — its X.509 credentials. The show-once private key is
   written to disk *before* anything else can fail
   (`<config>/device_certs/<thing>/`). This happens **before** flashing, so an
   API failure leaves the board untouched.
3. **NVS bake** — a per-board provisioning image: device name, IANA timezone
   (from this PC's clock), `flash_time` RTC seed, the board's own cert + key +
   Amazon Root CA 1, Wi-Fi credentials, MQTT broker (per environment), client
   id = openJII Thing name (required by the AWS IoT policy), topic root,
   command/status topics.
4. **Flash** — the latest GitHub release (`ambyte-iot-v*.zip`, cached locally,
   version shown in the GUI) + the NVS image, offsets from the release's own
   `flasher_args.json`. Only those regions are written — field data in
   coredump/littlefs/storage survives. A mid-way failure leaves the chip in
   the ROM bootloader (re-flashable) and enables **Retry flash**.
5. **RTC** — waits for the freshly booted console (it appears 20–35 s after
   reset, and the USB port may re-enumerate) and sets the exact current UTC
   epoch with `rtc set` (applies immediately).
6. **Verify** — reads back `cfg get device_name`, `cfg get timezone`, `rtc`
   and reports PASS/FAIL per item. On failure, **Retry provisioning** repairs
   name/timezone over the console (`cfg set` + reboot) and re-verifies —
   never a re-flash.

## Design notes / firmware facts this tool relies on

- Certificates and Wi-Fi credentials have **no console path** in the firmware
  (the cert setters are dead code; `wifi_join` doesn't set the provisioned
  flag) — NVS baking at 0x9000 (`0x6000` bytes, matches `partitions.csv`) is
  the provisioning protocol. The NVS generator is vendored
  (`vendor/nvs_partition_gen.py`, from ESP-IDF), so no IDF install is needed.
- `cfg get` returns **raw NVS strings**: a fleet-default board answers
  `device_name = AMBYTE_{MAC}` — the tool expands the token before comparing.
- The MQTT client id **must equal the openJII Thing name**
  (`ambyte_<MAC>`): the platform IoT policy renders identity-bound resources
  as `${iot:Connection.Thing.ThingName}`, which AWS only resolves when they
  match.
- The device timezone table is fixed (`components/timezone/timezone.c`); the
  GUI warns when this PC's zone is not in it (scheduling then falls back to a
  fixed UTC offset on the device).
- openJII auth: the API exposes **no password login and no OAuth device
  flow**; the supported desktop flow is a personal **API key** sent as
  `x-api-key`. Device-registry endpoints are additionally gated by the
  `iot-devices` feature flag per user — a 403 means the flag is off for your
  account.
- The AWS IoT broker endpoints in `config.py` are account-specific and not
  discoverable via any openJII API; they were resolved with
  `aws iot describe-endpoint --endpoint-type iot:Data-ATS` against each
  account. If an environment's endpoint ever changes, update it there — the
  GUI refuses to start a procedure for an environment whose endpoint is unset.

## Files

```
flash_gui/
  __main__.py        python -m flash_gui
  gui.py             Tkinter app (worker threads, never blocks the UI)
  procedure.py       the per-board state machine + retry paths
  config.py          environments, topic conventions, persisted settings
  release_fetch.py   GitHub release download + cache
  nvs_builder.py     per-board NVS image (identity/certs/Wi-Fi/RTC seed)
  esptool_ops.py     esptool>=5 scripting API (read MAC, flash, NVS-only)
  ambyte_serial.py   firmware console client (probe / cfg / rtc / verify)
  openjii_client.py  API-key auth, experiments, device registry, credentials
  timezones.py       host IANA zone + the firmware's supported-zone table
  vendor/            vendored ESP-IDF nvs_partition_gen.py
  tests/             pure-logic unit tests (no hardware/network)
```

Settings, release cache and issued certificates live in the per-user config
dir (`%APPDATA%\ambyte-flash-gui` on Windows, `~/.config/ambyte-flash-gui` on
Linux, `~/Library/Application Support/ambyte-flash-gui` on macOS). The API key
and Wi-Fi password are stored there in plain text with owner-only permissions
— treat that directory as sensitive.

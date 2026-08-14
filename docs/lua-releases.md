# Lua releases and rollout

Firmware and the field measurement script are independent release units in one repository:

| Unit | Paths | Tag | Assets |
|---|---|---|---|
| Firmware | Everything except `lua/**` | `vX.Y.Z` | `firmware.bin`, flash ZIP |
| Field Lua catalog | `lua/**` | `lua-vX.Y.Z` | One `.lua` asset and manifest per script |

The PR workflow squashes the proposed tree with the validated PR title, then previews both units. A change under `lua/**` can therefore publish Lua without creating a firmware release; a mixed PR can publish both. The main-branch workflow downloads the already verified PR artifact and publishes only the affected unit(s).

Every Lua release contains the complete script catalog. A change to any catalog script creates one new `lua-vX.Y.Z` release in which every selectable script has its own immutable asset and manifest:

| Workflow choice | Release assets | Intended use |
|---|---|---|
| `main` | `main.lua`, `main.lua.manifest.json` | Default multi-channel measurement schedule |
| `legacy_1Hz_spec` | `legacy_1Hz_spec.lua`, `legacy_1Hz_spec.lua.manifest.json` | One-channel, 1 Hz cmd 31 acquisition |

## Rollout payload

Each Lua manifest contains a ready-to-publish `script_update` object. For example, the default script uses:

```json
{
  "type": "script_update",
  "id": "lua-v1.0.0",
  "url": "https://github.com/Jan-IngenHousz-Institute/ambyte-iot/releases/download/lua-v1.0.0/main.lua",
  "checksum": "<sha256>",
  "script_version": "1.0.0",
  "built_against_fw": "1.1.0"
}
```

The alternate manifest points to `legacy_1Hz_spec.lua` and uses a distinct campaign ID such as `lua-v1.0.0:legacy_1Hz_spec`. Both are installed by the device as `/sdcard/main.lua`; the release asset name only selects the source payload.

Publish that object through the same device selection and command-topic mechanism used for firmware OTA. The device downloads in chunks, verifies the digest, parses the script without executing it, stages and flushes `main.lua.new`, retains `main.lua.bak`, atomically swaps, persists release identity, and reboots by default.

For the normal operator path, open **Actions → Fleet deploy (Lua) → Run workflow**, choose the release tag and the **Released Lua script** field, target the cohort, leave **dry run** enabled for the preview, then rerun live when the selection is correct.

`built_against_fw` is provenance: it identifies the firmware API surface used when the Lua asset was released. It is not an automatically inferred minimum-compatible version.

## Verification

The terminal `script_status` response and firmware-owned STATUS heartbeat expose:

- `app_version`: currently running compiled firmware.
- `script_sha256`: SHA-256 of the actual `/sdcard/main.lua` bytes.
- `script_version`: independent Lua release version, when verified.
- `script_built_against_fw`: firmware release used when packaging that Lua release.
- `script_installed_on_fw`: compiled firmware running when the script was installed.
- `script_metadata_verified`: true only when the actual file hash matches persisted release metadata.

A manual SD-card replacement deliberately reports its new hash with empty release fields and `script_metadata_verified=false`. This prevents stale version labels from being attached to untracked bytes.

Legacy remote pushes that omit `script_version` or `built_against_fw` behave the same way: the device still reports the exact active-file hash, but leaves the release fields empty and reports `script_metadata_verified=false`.

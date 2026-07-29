# Firmware candidate builder

This module builds and verifies the portable, unprovisioned firmware candidate.
It intentionally does not check out code, upload GitHub artifacts, create
releases, or publish to a fleet.

The composite action in `.github/actions/build-firmware-candidate/action.yml`
installs PlatformIO Core `6.1.19`. `platformio.ini` pins the Espressif platform,
ESP-IDF framework, and esptool package used by the build. The builder always
overrides the subprocess environment with `AMBYTE_NVS_SKIP=1` and the validated
`AMBYTE_PROJECT_VER`.

Accepted firmware versions are:

- a bare release version such as `1.1.0`; or
- `pr-<number>-<12 lowercase hex>`, derived from the positive PR number and the
  first 12 characters of the exact head SHA, such as `pr-42-deadbeefcafe`.

Candidate metadata uses `schemas/candidate-metadata.schema.json`. `artifact_id`
is deliberately absent: GitHub assigns it after upload. The manifest instead
binds `firmware-candidate-pr-<number>-<head-sha-prefix>` as its deterministic
artifact name plus the exact workflow run identity.
The manifest also binds the analyzed latest release tag and the exact failed
SHA for a verified recovery (otherwise `recovery_of_sha` is null), allowing the
main publisher to rederive the complete Ticket 1 predecessor decision rather
than trusting current mutable state.

Run the local tests with:

```sh
python3 -m unittest discover -s tools/firmware_candidate/tests -v
```

Verify a produced candidate with:

```sh
python3 tools/firmware_candidate/verify.py /path/to/candidate
```

The ZIP is intentionally stored without compression and with fixed timestamps,
so packaging does not depend on a runner's clock or compression library. The
inner manifest/checksum layer covers only ZIP payloads. The outer layer covers
the completed ZIP, standalone `firmware.bin`, and `release-notes.md`; neither
layer hashes itself.

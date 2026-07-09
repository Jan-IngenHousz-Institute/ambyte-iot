# OTA firmware update — staged plan

Host-triggered OTA for the **ambyte** (ESP32-S3 gateway), and later the
**ambit** sensors (ESP32-C3) over UART. Companion to
[device-script-delivery.md](../device-script-delivery.md), which describes the
Lua-script push that shares the same inbound channel.

> **Revised 2026-06-11 (a):** the trigger/orchestration layer was going to move
> to **AWS IoT Jobs**. The download path is unchanged either way (Stage-0-proven
> `esp_https_ota`).
>
> **Revised 2026-06-11 (b) — SHIP NOW via the custom topic; Jobs later.** The
> partner fixed the subscribe policy, so the **custom command topic works today**
> (`device/scripts/v1/…`, SUBACK rc=1). AWS Jobs, by contrast, still needs partner
> work the device can't self-serve: `$aws/things/<thing>/jobs/*` requires
> `client_id == thing name` (currently `client_id=AMBYTE_{MAC}` ≠ thing
> `dom_ludo_…`) **and** a separate jobs-topic policy grant. Since OTA is needed
> now, **Stage 3 ships triggered by the working custom topic** carrying a GitHub
> URL (the original idea); **Stage 2 (Jobs) is deferred** to a later swap of just
> the trigger transport. Everything else — Stage-1 dual-OTA partitions + rollback,
> `esp_https_ota` download, the Stage-3b Signer option — is unchanged and reused.
> The Jobs migration becomes: align `client_id` to thing name, get the jobs
> policy, repoint `mqtt_client` subscribe + the router from the custom topic to
> the `$aws/.../jobs/*` topics. The handler/download/rollback don't change.

## Decisions already taken

- **Trigger transport: AWS IoT Jobs** on the reserved
  `$aws/things/<thingName>/jobs/*` topics, over the existing esp-mqtt
  connection. A job's document carries the same payload the custom trigger
  would have (`type`-dispatched: `ota_update`, `script_update`, future
  commands).
- **Firmware source:** the job document carries the **full download URL**
  (any GitHub release asset or `raw.githubusercontent.com/<repo>/<branch>/…`),
  so we can point at an arbitrary branch or tag without reflashing. If the
  platform team prefers S3 hosting, job documents support
  `${aws:iot:s3-presigned-url:…}` placeholders — a drop-in swap later.
- **OTA mechanism:** application-level `esp_https_ota` into a dual-OTA
  partition layout, TLS validated by the Mozilla cert bundle, with rollback.

## Why Jobs instead of the custom trigger

1. **It dissolves the Stage-2 policy blocker.** The custom command topic is
   dead in the water because openJII's subscribe policy is scoped by a Cognito
   identity variable that is *empty* for cert-authenticated devices (the
   SUBACK 135 failure). Jobs topics are granted with the standard
   `${iot:Connection.Thing.ThingName}` thing-policy variable — the documented
   pattern for cert-auth fleets. Precondition (already true in our setup,
   enforced by their policy anyway): the device connects with **client ID =
   thing name** and the cert attached to the thing.
2. **The retain/idempotency hack becomes unnecessary.** Jobs have a real
   per-device execution lifecycle (QUEUED → IN_PROGRESS → SUCCEEDED/FAILED/
   TIMED_OUT). An offline device picks its QUEUED job up on reconnect — the
   problem retain-ON was solving — and an applied job never re-fires, the
   problem the NVS last-id dedupe was solving. The NVS latch survives in a
   smaller role (reboot correlation, below).
3. **Fleet visibility for free:** per-device status, statusDetails, timeouts
   (`inProgressTimeoutInMinutes` → auto-TIMED_OUT watchdog), thing-group
   targeting and staged rollout/abort configs in the AWS console — all things
   the custom status topic would have reimplemented poorly.

## Why NOT the full AWS OTA Update service (CreateOTAUpdate + OTA agent)

Assessed and deferred, not rejected on principle:

- The official OTA agent (`ota-for-aws-iot-embedded-sdk` via `esp-aws-iot`)
  is built on **coreMQTT/coreHTTP**, not esp-mqtt. Adopting it means either a
  full MQTT-stack migration or a second TLS session — the latter is
  heap-prohibitive on this board (~17 KB steady-state largest block), the
  former a rework far larger than OTA itself. AWS's own current guidance has
  shifted toward exactly the Jobs-based custom flow we're adopting.
- `CreateOTAUpdate` adds AWS Signer **code signing** — real authenticity, and
  the honest answer to correction #4 below. It layers cleanly on top of a
  Jobs-based device flow later (verify the signature file referenced by the
  job document before `esp_ota_set_boot_partition`). Park it with Secure Boot
  v2 as the hardening path.

## Corrections carried over from the original critical review

1. **The OTA must run in its own task**, not in the MQTT event callback (a
   multi-minute blocking download there stalls the MQTT event loop /
   keepalive). The callback only parses → validates → signals.
2. ~~Retain + NVS-dedupe idempotency~~ — superseded by the job execution
   lifecycle (see above). The NVS jobId latch remains for **reboot
   correlation only**: it tells the freshly-booted image which execution to
   report on.
3. **`mark_app_valid` is gated on connectivity:** the new image only cancels
   rollback after it has reconnected to MQTT — at which point it also reports
   the job SUCCEEDED. A connectivity-breaking update rolls back instead of
   stranding the device, and the rolled-back image reports the job FAILED.
4. **`sha256` + `size` in the job document are integrity, not authenticity.**
   The trust boundary becomes "who can create jobs in the openJII AWS
   account" (IAM) — strictly better than "who can publish to a topic", but
   still not signed images. True authenticity = AWS Signer and/or Secure
   Boot v2, out of scope for now.

---

## Stage 0 — TLS/heap feasibility spike  ⟵ *PASSED 2026-06-10 on hardware*

**Question it answered (make-or-break):** does an HTTPS firmware download from
GitHub work on this board? **YES.** A full 1.36 MB esp32s3 image downloaded from a
public GitHub release (`protoMUSIC/releases/ota-spike-test`) end-to-end:
- TLS handshake validated against **both** github.com AND the Fastly CDN across the
  302 redirect (cert bundle), `complete_image = YES`, ~53 KiB/s.
- **Heap floor 131 KB largest-internal, flat** the entire download (measured in the
  quiesced pre-MQTT state Stage 3 will reproduce) — heap is a non-issue.

**Proven production settings (carry into the Stage-3 handler / config):**
1. `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` + `_DEFAULT_FULL=y` — validates github.com
   + the CDN host.
2. `esp_http_client_config`: **`buffer_size=4096`, `buffer_size_tx=4096`** — the 512 B
   defaults overflow on GitHub's long signed-redirect URL + verbose CDN headers
   (`HTTP_CLIENT: Out of buffer`).
3. **`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384`** (raised back from 8192) — S3/Fastly
   stream the body in full 16 KiB TLS records; with `DYNAMIC_BUFFER` on, a record
   over the cap fails on the first data read with `-0x7100` (BAD_INPUT_DATA). Safe
   to raise because `DYNAMIC_BUFFER` makes it a *transient per-record ceiling*: MQTT's
   small records still alloc small. **Caveat to verify:** this is now in
   `sdkconfig.defaults` so it affects NORMAL builds too — confirm steady-state MQTT is
   unaffected (watch the `sync_runner` "heap:" line) after reflashing normal firmware.

**What landed:** the cert-bundle config in `sdkconfig.defaults` (kept — the
real OTA path needs it). The throwaway `components/ota_spike/` harness, its
`-DSPIKE_OTA` build flags, and the `app_main.c` hook were **removed** once
Stage 3 shipped and was HW-validated (the real `ota_update` component
superseded them).

**Gate: PASSED → GitHub-hosted OTA is viable; proceed to Stage 1.**

---

## Stage 1 — Partition migration + rollback  ⟵ *IMPLEMENTED 2026-06-11 on branch
`feature/ota-stage1-partitions` (build passes; NOT yet flashed)*

The v1 `partitions.csv` had `factory + ota_0` with **no `otadata` / `ota_1`** —
not OTA-capable. Reworked to the dual-OTA layout:

```
nvs       0x9000   0x6000     (unchanged — provisioning image flashes here)
otadata   0xf000   0x2000     (NEW — bootloader slot selector)
phy_init  0x11000  0x1000     (moved; RF cal self-heals)
ota_0     0x20000  0x2F0000   (2.94 MB; app boots here; ~44% used)
ota_1     0x310000 0x2F0000   (2.94 MB; OTA target slot)
coredump  0x610000 0x10000    (unchanged)
littlefs  0x620000 0x80000    (unchanged — field data preserved)
storage   0x6A0000 0x960000   (unchanged — field data preserved)
```

- `littlefs`/`storage`/`coredump` keep their v1 offsets, so a migration reflash
  that does **not** `erase_flash` preserves internal-flash field data. (Physical
  SD-card data — the event log — is on a separate medium and always survives.)
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` added. Harmless for esptool-flashed
  apps (they boot VALID); only OTA images boot PENDING_VERIFY and need
  `esp_ota_mark_app_valid_cancel_rollback()` (wired in Stage 3, gated on MQTT
  reconnect). 56 KiB gap before `ota_0` is 64 KiB app-partition alignment.
- The build emits **`ota_data_initial.bin`** (empty/0xFF) → the flasher writes
  it to `otadata`, so the bootloader cleanly boots `ota_0` after migration; no
  stale-otadata hazard.

**Migration procedure (the one-time hand-reflash per fielded unit):**
- A normal `pio run -t upload` writes bootloader + partition table +
  `ota_data_initial` (→ `otadata`) + app (→ `ota_0`) + the NVS image (@0x9000),
  and does **not** `erase_flash` — so `littlefs`/`storage` survive. NVS runtime
  keys (cursor/next_id) are re-seeded as usual; the event log reseeds `next_id`
  from the SD card.
- Do **NOT** `erase_flash` (it would wipe internal field data). If a unit fails
  to boot the new slot, erase only the `otadata` region (`0xf000 0x2000`) and
  re-upload — don't whole-chip erase.
- You cannot OTA *into* this layout — every already-fielded ambyte needs this
  physical reflash once. OTA only helps units flashed after the migration.

**Remaining for Stage 1 closure:** flash the branch to a bench unit and do the
manual `esp_ota` round-trip (write `ota_1`, `esp_ota_set_boot_partition`,
reboot, confirm it boots `ota_1`, mark valid) to prove the dual-slot + rollback
config end-to-end before Stage 2/3 build on it.

---

## Stage 2 — Jobs channel  *(rework of the built custom channel)*

Repoint the existing inbound infrastructure (connect-time subscribe +
multi-part reassembly in `mqtt_client.c`; JSON router in `command_router/`)
from the custom command topic to the Jobs reserved topics. Hand-rolled on
esp-mqtt + cJSON — the AWS Jobs "library" is mostly topic-string helpers; we
don't need it.

**Device flow:**
- On `MQTT_EVENT_CONNECTED`: subscribe (QoS 1) to
  `$aws/things/<tn>/jobs/notify-next`,
  `$aws/things/<tn>/jobs/start-next/accepted` + `/rejected`,
  `$aws/things/<tn>/jobs/+/update/accepted` + `/rejected`;
  then publish `{}` to `$aws/things/<tn>/jobs/start-next` to claim any QUEUED
  execution (this is how offline-queued jobs are picked up — no retain).
- `start-next/accepted` carries the **execution + job document**; the router
  dispatches on the document's `type` exactly as designed for the custom
  channel. Claiming via start-next marks it IN_PROGRESS server-side.
- Replies become `UpdateJobExecution` publishes
  (`$aws/things/<tn>/jobs/<jobId>/update` with `status` + `statusDetails`)
  instead of the custom status topic.
- Reassembly stays (esp-mqtt chunks inbound > ~1 KB; job documents are small
  but multi-KB). **Job documents are capped at 32 KB** — see Stage 4 for the
  consequence on script delivery.
- `handle_ping` survives as a no-op job type (`{"type":"ping"}` → report
  SUCCEEDED with uptime/fw in `statusDetails`) — the Stage-2 end-to-end proof.

**Policy ask to the platform team** (replaces the Cognito-policy fix):
`iot:Subscribe`/`iot:Receive` on
`$aws/things/${iot:Connection.Thing.ThingName}/jobs/*` topic filters and
`iot:Publish` on the matching topics. Standard AWS pattern, resolves for
cert auth because client ID = thing name. Plus: whoever triggers updates
needs IAM `iot:CreateJob` (+ console or a small boto3 script — replaces
`docs/stage2_command_test.py`, which becomes a job-creation/monitor tool).

**Cleanup:** `AMBYTE_COMMAND_TOPIC` / `AMBYTE_STATUS_TOPIC` env keys and their
NVS plumbing go away (jobs topics derive from the thing name = client ID).

**Deliverable:** create a `ping` job in the console → device claims it,
reports SUCCEEDED with statusDetails visible in the console. No OTA yet.

---

## Stage 3 — ambyte self-OTA handler  ⟵ *IMPLEMENTED 2026-06-11 on branch
`feature/ota-stage1-partitions` via the CUSTOM command topic (build passes; NOT
yet HW-tested). Jobs-triggered variant deferred — see the Revised(b) note.*

Component `ota_update` (`components/ota_update/`), dispatched by `command_router`
on an `ota_update` command on the custom command topic:

```json
{ "type": "ota_update", "id": "<unique-each-time>",
  "url": "https://github.com/<owner>/<repo>/releases/download/<tag>/firmware.bin" }
```

Flow: `command_router` dedupes on `id` (already-applied → ignore) and marks it
applied up front (a retained/duplicate trigger can't re-OTA) → `ota_update_request`
→ worker task: report `accepted` → **latch `id` in NVS** → `comms_suspend`
(`mqtt_client_stop`, freeing the TLS heap — the board can't hold two TLS
sessions) → `esp_https_ota(url)` with the Stage-0 settings (cert bundle, 4 KiB
HTTP buffers, follows the 302) → `esp_restart` → new image boots PENDING_VERIFY
→ on MQTT reconnect, `esp_ota_mark_app_valid_cancel_rollback()` + report
`success` (with the running `fw` version) → clear latch. Failure paths: download
error → clear latch, `comms_resume`, report `failed`; image boots but can't
reconnect within 5 min → `esp_ota_mark_app_invalid_rollback_and_reboot()`
(reverts to the known-good slot).

Status JSON on the status topic:
`{"type":"ota_status","device_id":…,"id":…,"state":"accepted|failed|success","fw":"<version>"[,"detail":…]}`.

Integrity today = HTTPS + the esp-image validation `esp_https_ota` performs.
Authenticity (AWS Signer) is **Stage 3b**, not yet wired — so the trust boundary
is "who can publish to the command topic."

### Operating — triggering an OTA

1. The **target `firmware.bin` must be built from an OTA-capable branch** (dual-OTA
   partitions + rollback + this `ota_update` handler). An image without the
   handler boots but never marks itself valid → rolls back after 5 min.
2. Publish to the device's command topic (the one that SUBACKs rc=1):
   ```json
   { "type":"ota_update", "id":"ota-2026-06-11-1",
     "url":"https://github.com/<owner>/<repo>/releases/download/<tag>/firmware.bin" }
   ```
   - `url`: a **public** HTTPS app-image. Two working forms:
     - a GitHub **Release asset**: `https://github.com/<o>/<r>/releases/download/<tag>/firmware.bin`
       — only resolves if an actual Release with that tag has the file attached
       (404 = no such Release/asset). A repo *folder* named `releases/…` is NOT
       a Release.
     - a **file committed in the repo**, via the raw host:
       `https://raw.githubusercontent.com/<o>/<r>/<branch>/<path>/firmware.bin`.
     NOT working: `github.com/...(/tree/|/blob/)...` — those are the web file
     browser and serve HTML (rejected by the device with a bad_url / not-an-image
     error).
   - `id`: latched only on a **successful** update (to stop a retained trigger
     re-OTAing). A failed attempt is *not* latched, so you can re-send the same
     `id` to retry; only a completed update locks its id (use a new one next time).
3. Watch the status topic for `accepted` → (download, ~1 min) → after reboot,
   `success` with the new `fw`. Serial shows `firmware build tag:` + the new
   boot offset.

### Stage-3 HW test (step by step)

Two images from the same branch, distinguished by `AMBYTE_FW_TAG` (main/app_main.c):

1. **Build + flash image A (the migration flash):** set `AMBYTE_FW_TAG "ota-A"`,
   `pio run -t upload` (a normal upload — **never `erase_flash`**). Confirm the
   boot log: `Loaded app from partition at offset 0x20000`, `firmware build tag:
   ota-A`, and `event_log: ready … pending=…` (field data preserved).
2. **Build + publish image B (the OTA target):** set `AMBYTE_FW_TAG "ota-B"`,
   `pio run` (no upload). Create a **public** GitHub release and attach
   `.pio/build/esp32-s3-devkitm-1/firmware.bin`. Copy the asset download URL.
3. **Trigger:** publish the `ota_update` command (above) with that URL. Expect
   `ota_status state=accepted` on the status topic, then the serial log: MQTT
   disconnect → `esp_https_ota` download progress → reboot. (Re-sending the same
   `id` after a failed attempt is fine — only a *successful* update locks the id.)
4. **Confirm the swap:** new boot log shows `Loaded app from partition at offset
   0x310000` (= `ota_1`) and `firmware build tag: ota-B`. After MQTT reconnects,
   `ota_status state=success fw=<B version>`. → OTA downloaded + ran the new bits.
5. **`ota_status`** at the console: `running: ota_1 … state=VALID` (mark-valid
   committed it). A second OTA would target `ota_0`, ping-ponging slots.
6. **Failure path:** trigger an OTA with a bogus `url` (404 / not an image) and a
   fresh `id` → `ota_status state=failed`, device stays on the current image
   (no reboot). (The *connectivity*-rollback path is already proven by the
   Stage-1 `ota_selftest` round-trip; reproducing it here needs a deliberately
   unconnectable image.)

**Deliverable:** publish an `ota_update` → device self-updates from a GitHub
release, boots the new slot, confirms `success`; a bad URL reports `failed` and
stays put.

---

## Stage 3b — image authenticity via AWS Signer  *(hardening; layers on Stage 3)*

This is the part of "the dedicated AWS IoT OTA feature" worth adopting without
the full OTA-agent stack migration. Stage 3 gives integrity (`sha256`/`size`)
but not **authenticity** — the trust boundary is only "who can create jobs in
the AWS account" (IAM). AWS Signer closes that with a real signature over the
image, and it grafts onto the **existing Jobs flow** rather than requiring
coreMQTT/coreHTTP or a second TLS session (the reason the full managed OTA
service is deferred — see the "Why NOT" note above).

**Why it fits the Jobs flow:** `CreateOTAUpdate` already produces a code-signed
artifact + a signature, and the job document can carry the signature (or a URL
to it) alongside the image URL. The device just adds one verify step; the
transport stays esp_https_ota over the existing connection.

**Scope:**
- Sign the firmware with **AWS Signer** (or an offline ECDSA key) at release
  time; publish the signature next to the `firmware.bin`.
- Provision the **public key** on the device (NVS or embedded in the app) —
  one-time, like the cert bundle.
- Job document gains `sig` (+ `sig_alg`). The Stage-3 handler verifies the
  downloaded image against the public key **before** `esp_ota_set_boot_partition`
  — using `esp_https_ota`'s post-download hook or a streaming hash + an mbedTLS
  ECDSA verify (mbedTLS is already linked for TLS, so no new heap stack).
- A bad/unsigned/altered image is rejected and the job reports FAILED
  `{"reason":"signature"}` — never boots.

**Relation to Secure Boot v2:** Signer verifies *who built it* (app-level,
software trust). Secure Boot v2 verifies *what the ROM will boot* (hardware
root of trust, efuse-burned). They're complementary; Signer is the cheaper
first step and needs no efuse changes. Full hardware authenticity = Secure
Boot v2, still a separate, irreversible (efuse) decision.

**Deliverable:** an image signed by the release pipeline updates normally; a
tampered or unsigned image is refused before boot and the job shows FAILED with
a signature reason. No coreMQTT migration, no second TLS session.

---

## Stage 4 — Lua `script_update` as a job  *(parallel to Stage 3)*

`type: "script_update"` per
[device-script-delivery.md](../device-script-delivery.md), with one
structural change: the 32 KB job-document cap means the **inline-script
variant dies** (scripts can be 128 KB). The document carries a `url` +
`checksum` instead, and the device fetches the script over HTTPS exactly like
firmware — which also deletes the only consumer of >32 KB MQTT reassembly.
Verify checksum → write to SD/littlefs → restart `lua_runner` → report
SUCCEEDED. `device-script-delivery.md` needs a matching revision when this
lands. Note the **firmware ↔ script compatibility contract** — an OTA and a
script push can each break the other if their API versions drift.

---

## Stage 5 — ambit OTA over UART  *(later; separate workstream)*

Out of scope here. Download `.bin` to the ambyte SD → framed/CRC'd/ACK'd UART
chunk protocol → Arduino `Update` on the C3 (which already has dual-OTA partitions
and is USB-JTAG recoverable). Same jobs channel, `target: "ambit"` in the job
document reserves the slot; long-running executions suit jobs well
(IN_PROGRESS statusDetails per chunk phase, generous timeout). See the
OTA-plan memory and ambit-firmware-interop notes.

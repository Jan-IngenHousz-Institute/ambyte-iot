# Pushing `main.lua` to a live device over MQTT (`script_update`)

Replace a device's `/sdcard/main.lua` **remotely, with no reflash and no SD removal**,
by publishing one JSON message. The firmware syntax-checks the script, stages it,
stops the Lua runner, swaps the file (keeping the old one as `main.lua.bak`),
restarts the runner, and replies on the status topic.

**Delivery is by raw URL** — the command carries just a link, and the device
streams the script over HTTPS. This is the default because it works on any heap
state (even mid-measurement); inline embedding is a fallback for tiny scripts and
lives in [Appendix A](#appendix-a). This runbook uses the example script at
[`docs/example_SDfolders/main.lua`](example_SDfolders/main.lua) as the concrete
payload, but the process is identical for any script.

> **Contract reference:** the field/behaviour contract lives in
> [`device-script-delivery.md`](../planning-internalDocs/device-script-delivery.md). This doc is the
> hands-on procedure and was validated on hardware 2026-07-14.

---

## 0. How it works (30-second model)

- `script_update` arrives on the device's **command topic** — the same inbound
  channel as `ping`/`ota_update` (dispatched by `components/command_router`). On
  this deployment the command topic happens to be
  `device/scripts/v1/Ambyte/2/AMBYTE_{MAC}`, but *any* command type is handled on
  whatever the command topic is set to.
- **URL delivery (default):** the command is tiny — just `{type, id, url, checksum}`.
  Because the message is small it is received on any heap state; the device then
  stops **both** Lua and MQTT (defragmenting the heap and freeing it for the
  download's TLS), **streams** the script over HTTPS in 4 KB chunks — no large
  contiguous allocation, ever — then reconnects and applies. Same reliability as
  OTA. (Contrast inline embedding, [Appendix A](#appendix-a): the whole ~8 KB
  script rides in one MQTT message that needs a contiguous ~8 KB TLS buffer a busy
  device often can't allocate, so large inline pushes silently drop.)
- The device replies on its **status topic** with a `script_status` message.
- Apply order (`components/script_update/script_update.c`): **download** (URL) →
  **sha256** (if a checksum is supplied) → **Lua syntax check** (parse-only, before
  the SD is touched) → write `main.lua.new` + fsync → stop runner → keep old script
  as `main.lua.bak` → atomic rename → **latch the `id` in NVS (success only)** →
  **reboot into the new script** (the default). Send `"reboot": false` to keep the
  old in-place behaviour instead (swap + restart just the Lua runner, no reboot).
- **Reboot by default:** a successful update **restarts the whole device** so the
  new script runs from a clean boot — expect the device to drop offline for a few
  seconds and reconnect on the new `main.lua`. The `id` is latched *before* the
  reboot, so a **retained** update can't loop the reboot (it dedupes on reconnect).

### Prerequisites for URL delivery

1. The script must be reachable at a **direct raw URL** (public, no auth — the
   device fetches anonymously) — a `raw.githubusercontent.com/...` link, **not** a
   github `/blob/` page. For this repo that is
   `https://raw.githubusercontent.com/<owner>/<repo>/<branch>/<path>`; the builder
   in Step 2 derives it for you.
2. **Commit *and* push the script first.** GitHub raw serves the *pushed commit*,
   not your working copy — an un-pushed edit means the device downloads stale bytes
   (and fails the checksum). The builder warns when the file is dirty or unpushed.

### The three gotchas that bite people

1. **`id` must be unique per push.** A successful apply latches the `id` in NVS;
   re-sending the same `id` logs `already applied — ignoring` and **sends no reply**.
   Use a fresh id every time (the builder auto-stamps a UTC timestamp).
2. **The topic contains a live `{MAC}` placeholder.** `cfg get command_topic`
   shows the *stored template* (`…/AMBYTE_{MAC}`); the firmware expands `{MAC}` to
   the board's Wi-Fi STA MAC **at boot, in RAM only**. You must publish to the
   **expanded** topic (real MAC, colons, **no trailing slash**).
3. **The AWS IoT policy must authorize the exact topics** — Subscribe on the
   command topic *and* Publish on the status topic (different namespaces here).
   If the reply never arrives but the serial log says `applied`, that's a policy
   gap on the status topic, not an update failure.

---

## 1. Get the device's real topics

Connect to the USB serial console and run:

```
status
```

Find the line:

```
 - MAC: E8:F6:0A:B1:1F:34
```

That is your 6-octet Wi-Fi STA MAC, formatted exactly as `{MAC}` expands (uppercase
hex, colons). Build the two topics by substituting it for `{MAC}` in the stored
templates (from `cfg get command_topic` / `cfg get status_topic`):

| Purpose | Topic (example — use YOUR MAC) |
|---|---|
| **Publish** the update (command topic) | `device/scripts/v1/Ambyte/2/AMBYTE_E8:F6:0A:B1:1F:34` |
| **Subscribe** for the reply (status topic) | `experiment/data_ingest/v1/665b6b18-3cfe-4d0a-85c7-3e84fa2f7834/multispeq/v1.0/AMBYTE_E8:F6:0A:B1:1F:34/status` |

**No trailing slash** on either. If you'd rather copy the fully-expanded strings
verbatim, `reboot` and read them from the boot log:

```
command router wired (cmd topic: device/scripts/v1/Ambyte/2/AMBYTE_E8:F6:0A:B1:1F:34)
subscribing to device/scripts/v1/Ambyte/2/AMBYTE_E8:F6:0A:B1:1F:34 (msg_id=…)
```

(These are firmware log **output** — don't type them at the prompt.)

---

## 2. Commit, push, and build the URL command

**First commit and push** your `main.lua` to the branch the raw URL points at
(the device downloads the pushed commit, not your working tree):

```bash
git add docs/example_SDfolders/main.lua
git commit -m "update main.lua"
git push
```

Then build the command. The helper [`build_script_payload.py`](build_script_payload.py)
— **standard-library Python 3.8+, no dependencies, any OS** — auto-derives the raw
URL from the git remote + current branch + file path, computes the SHA-256 over the
exact hosted bytes, and writes a tiny `payload.json`:

```bash
python3 docs/build_script_payload.py docs/example_SDfolders/main.lua
```

```
mode          = URL (auto-derived from git)
url           = https://raw.githubusercontent.com/<owner>/<repo>/main/docs/example_SDfolders/main.lua
id            = main-lua-url-20260718-192658
script bytes  = 6730   (CRLF->LF normalized)
sha256        = 7e04cc1b09e8fb63796db2a594d51207f27294329d09a49b774d2d1051efe785
reboot        = yes (device default)
message bytes = 253 / 16384   OK, within cap
-> wrote payload.json
```

`payload.json` is just:

```json
{"type":"script_update","id":"main-lua-url-20260718-192658","url":"https://raw.githubusercontent.com/<owner>/<repo>/main/docs/example_SDfolders/main.lua","checksum":"7e04cc1b09e8fb63796db2a594d51207f27294329d09a49b774d2d1051efe785"}
```

**The builder warns if the URL would serve stale bytes** — `!! WARNING: … has
UNCOMMITTED changes …` or `… ahead of origin/<branch> …`. Fix that (commit + push)
before publishing, or the device fails on `sha256 mismatch`.

Options:

| Flag | Effect |
|---|---|
| *(none)* | URL command; raw URL auto-derived from git, fresh `main-lua-url-<UTC>` id |
| `--url RAW_URL` | URL command with an explicit host (non-GitHub, gist, S3, …) instead of the git-derived one |
| `--inline` | embed the script in the message instead (fallback — see [Appendix A](#appendix-a)) |
| `--id ID` | set the update id explicitly (reusing an applied id is ignored by the device) |
| `--no-checksum` | omit the `checksum` field (it's optional; a bad download still fails the syntax check) |
| `--no-reboot` | add `"reboot": false` — swap in place without rebooting (the device default is to reboot) |
| `-o FILE` / `-o -` | write to FILE, or `-` for stdout (pipe to clipboard: `\| clip` / `\| pbcopy` / `\| xclip -sel c`) |

**Verify the hosted bytes** match the checksum (authoritative — hashes what the
device will actually download):

```bash
curl -sL '<raw-url>' | sha256sum          # or, fully offline, hash the pushed blob:
git show origin/<branch>:docs/example_SDfolders/main.lua | sha256sum
```

**Notes**
- **`checksum` is optional** (`--no-checksum`); when present it's verified, when
  absent it's not. Recommended to keep it — it also catches a truncated download.
- The `script` field is canonical for inline; `payload` is accepted as a legacy alias.

---

## 3. Subscribe for the reply

In the **AWS IoT console → MQTT test client → Subscribe to a topic**:

- Topic: your **status** topic from Step 1 (real MAC, ends in `/status`).
- QoS 1. Subscribe.

Leave it subscribed before you publish so you catch the `script_status` reply.

---

## 4. Publish the update

In **MQTT test client → Publish to a topic**:

- **Topic name:** your **command** topic from Step 1 (real MAC, **no trailing slash**).
- **Message payload:** paste the contents of `payload.json` (open the file, or send
  it to the clipboard as shown in Step 2).
- **Additional configuration:**
  - **Retain: OFF** (recommended for a targeted push to an online device — nothing
    lingers). Use Retain **ON** only if you want an *offline* device to receive it
    on next reconnect — but then you **must clear it afterwards** (Step 6).
  - QoS: the console publishes at QoS 0; that's fine — the device subscribed at
    QoS 1 and the broker delivers at the per-hop minimum.
- **Publish.**

The device must be **online** (connected to AWS IoT) for a non-retained publish to
reach it, and it needs external power / connectivity to reach the URL. During the
download it stops **both** Lua and MQTT, so expect a brief MQTT gap; by default it
then **reboots** right after applying (the `applied` reply is published first). Add
`"reboot": false` (or `--no-reboot`) to swap in place without rebooting.

---

## 5. Verify

**Serial console (authoritative — independent of whether the MQTT reply reaches you).**
By default the command lands, the device downloads + applies, and **reboots** into
the new script:

```
mqtt_client: inbound <M> B on device/scripts/v1/Ambyte/2/AMBYTE_E8:F6:0A:B1:1F:34
cmd_router:  command type=script_update id=main-lua-url-...
cmd_router:  script_update(url) id=… dispatched (https://…, reboot)
script_upd:  script_update(url) id=…: https://…
script_upd:  downloaded <N> bytes, sha256=…
script_upd:  main.lua replaced from url (<N> bytes); previous kept as /sdcard/main.lua.bak — rebooting to run it
   ... device reboots: boot banner + ordered-boot logs ...
```

After the reboot the new schedule starts — for the bundled example:

```
dev_cmd: schedule started; sunrise=HH:MM sunset=HH:MM
dev_cmd: SS ch0: <points> points, <temp>C, stored 1     (steady-state)
```

**Status topic (your AWS subscription)** — published just before the reboot:

```json
{"type":"script_status","device_id":"03:25:07:04","id":"main-lua-url-...","state":"applied","detail":"<N> bytes; rebooting"}
```

`state:"applied"` = success. If the update was **retained**, the device re-receives it
on reconnect and logs `already applied — ignoring` — no second reboot (the success-latch).

**With `"reboot": false`** the device does *not* restart: the serial log reads
`main.lua replaced (<N> bytes) + runner restarted` and the reply `detail` is just
`"<N> bytes"`. Any `state:"failed"` → see the [failure reference](#failure-reference).

---

## 6. Clean up & rollback

**If you published with Retain ON**, clear the retained message so it doesn't
re-deliver on every reconnect (harmless while the id stays latched, but it *will*
re-apply if NVS is ever wiped — e.g. a `nvs.bin` reflash resets the latch):

- Publish an **empty payload** with **Retain ON** to the same command topic.

**Rollback:** the previous script is always preserved as `/sdcard/main.lua.bak`.
To revert, either:

- push the previous script again via this same process (with a **new `id`**), or
- on the serial console: `lua stop`, restore the `.bak` over `main.lua` (pull the
  SD or use a file tool), then `lua start`.

---

<a id="appendix-a"></a>
## Appendix A — inline fallback (small scripts / no host)

When you can't host the file (no push access, private host the device can't reach,
a quick one-off), embed the script directly with `--inline`:

```bash
python3 docs/build_script_payload.py docs/example_SDfolders/main.lua --inline
```

This normalizes line endings, hashes the exact bytes it puts in the message, and
writes a `payload.json` containing the whole script:

```json
{"type":"script_update","id":"main-lua-...","script":"<lua>","checksum":"<sha256>"}
```

Publish it exactly as in Steps 3–5. **Caveats that make URL the default:**

- **Size limit:** the whole MQTT message must be **≤ 16384 bytes**
  (`INBOUND_MSG_LARGE_MAX`). The builder refuses to write over that.
- **Contiguous-heap fragility (the real killer):** any message over **2048 bytes**
  (`INBOUND_MSG_MAX`) needs a contiguous transient heap buffer sized to the message.
  A fragmented heap (mid-measurement) often can't provide it, so the push **silently
  drops** — serial shows `inbound message <N> B > cap 16384 (or no heap) — dropped
  (topic=…)` or `Dynamic Impl: alloc(…) failed / -0x7F00`, and **no reply arrives**.
  This is exactly what URL delivery avoids. If an inline push produces no reply,
  don't retry it — switch to URL.
- `--ascii` escapes non-ASCII to `\uXXXX` for channels that might mangle UTF-8
  (larger message; checksum unchanged). `--no-reboot` / `--no-checksum` work here too.

---

## Failure reference

Every `state:"failed"` reply carries a `detail`; the same cause is logged under the
`script_upd` tag on serial. The script is **left untouched** for every rejection
that happens before the swap.

| `detail` | Cause | Fix |
|---|---|---|
| `download failed (<err>)` | *(URL only)* unreachable URL / non-200 / short or truncated read | check it's a direct **raw** link (not `/blob/`), public, and the device has connectivity |
| `sha256 mismatch` | checksum ≠ sha256 of the downloaded/decoded script | you likely didn't push, or pushed different bytes — `git show origin/<branch>:<path> \| sha256sum` and regenerate; or omit `checksum` |
| *(a Lua parse error)* | script fails the parse-only syntax check | fix the syntax; note **runtime** errors are NOT caught here — a script that parses but crashes at run reports `applied`, then fails in the runner → recover via `main.lua.bak` |
| `reboot requires an id` | a reboot command (the default) with no `id` — un-dedupable, would boot-loop | include a unique `id` (the builder always does) or set `"reboot": false` |
| `SD card not mounted` | SD failed to mount at apply time (e.g. `sdmmc 0x107` timeout) | the id is **not** latched on this failure, so it stays retryable — re-publish once the SD is healthy |
| `cannot open /sdcard/main.lua.new` / `SD write failed` | SD write error mid-stage | check the card; retry |
| `lua task busy; retry` | runner stuck in a long C call, didn't stop within 5 s | wait a moment and re-publish |
| `rename failed` | FATFS rename failed (rare) | retry; old script auto-restored |
| `script installed but runner restart failed` | *(reboot:false only)* file swapped but runner wouldn't start | `lua start` on the console, or reboot |

Also note: `already applied — ignoring` (info log, **no reply**) means you reused an
`id` that was already applied — bump the `id`.

---

## Appendix B — local/scriptable alternative (mosquitto)

For a repeatable push without the console, use the device cert bundle under
`device_certs/<bundle>/` (same auth the device uses; give the test client a
*distinct* client id):

```bash
mosquitto_pub \
  --cafile device_certs/<bundle>/AmazonRootCA1.pem \
  --cert   device_certs/<bundle>/<hash>-certificate.pem.crt \
  --key    device_certs/<bundle>/<hash>-private.pem.key \
  -h <endpoint>-ats.iot.eu-central-1.amazonaws.com -p 8883 \
  -i <thing>-pusher \
  -t 'device/scripts/v1/Ambyte/2/AMBYTE_E8:F6:0A:B1:1F:34' \
  -q 1 -f payload.json
```

Subscribe for the reply in another terminal with `mosquitto_sub` on the status
topic (same certs). If your IoT policy pins `iot:Connect` to the thing-name client
id, the pusher needs its own permitted client id or cert/policy.
See `docs/stage2_command_test.py` for a Python client that already handles the
cert/TLS/`.env` plumbing (it sends `ping`; adapt the payload to `script_update`).

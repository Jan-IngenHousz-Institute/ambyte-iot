# HW test runbook — `feature/publish-reliability-hardening`

Gate checklist to run on a real unit before merging to `main`. Covers commits
`f8fea64` (in-flight-slot reaper + MQTT-disconnect clear) and `ce297ce`
(connectivity watchdog, sys_evt stack bump, cert provisioning, `AMBYTE_{MAC}`
rename), plus the new `evlog rewind` command.

The synthetic reaper (`inflight stall`) is the ONLY path validated so far
(README "Hardware-verified"). Everything below marked **[GATE]** is unproven
against a real transport and must pass before merge.

---

## 0. Setup / prerequisites

- Unit flashed from this branch with a **valid cert bundle + `.env`** (see §8 —
  the build warns if `AMBYTE_CLIENT_ID` ≠ the thing the cert is bound to).
- **External power** (USB/solar-in, not battery). The drain and the watchdog only
  run while `sync_runner_is_allowed()` = external power + no measurement in
  progress. On battery nothing drains and the watchdog never fires.
- **Valid RTC clock** (year ≥ 2024) — the watchdog and publish gate require it.
  Check/set with `rtc`.
- Serial console open (this is the only MQTT-state view — there is no
  `mqtt_status` command; watch the log lines).
- A host subscriber to confirm delivery + spot duplicates:
  `uv run docs/mqtt_tls_test_client.py --subscribe`
- A way to disrupt the link at two layers independently:
  - **Broker/return path only** (Wi-Fi stays associated) — a firewall/NAT rule on
    the hotspot/router blocking the broker's IP:8883 or dropping broker→device
    packets. Needed for [GATE] and the MQTT-disconnect test.
  - **Whole AP** — just power off the access point. For the Wi-Fi-drop test.

### Log strings to watch
| line | meaning |
|---|---|
| `MQTT connected` / `MQTT disconnected` | transport up/down |
| `Wi-Fi disconnected — stopping MQTT` | Wi-Fi STA drop path |
| `reaped stale in-flight publish (id=…)` | 60 s reaper fired (id=-1 ⇒ synthetic) |
| `cursor rewound to seq=… off=0` | `evlog rewind` took effect |
| `drain stalled waiting for PUBACK` | **BAD** — the wedge this branch fixes |
| `connectivity watchdog … rebooting` | 1 h liveness watchdog fired |

### Commands used
`status` · `rtc` · `record_env` (mint one event) · `evlog` / `evlog rewind [seq]`
· `inflight` / `inflight stall` · `netwd` / `netwd test` · `reboot`

---

## 1. Baseline (healthy link)

```
status                # peripherals + RTC time OK
evlog                 # note cursor ev-NNNNNN.log @ off, pending, next_id
inflight              # expect: idle
netwd                 # allowed=yes, clock=yes, would reboot now=no
record_env            # mint one T/H/P event
evlog                 # pending +1, then should return to prior as it drains
inflight              # briefly shows a real msg_id/measure_id, then idle on PUBACK
```
**Pass:** the event publishes, `evlog` pending returns to baseline, host subscriber
receives it, `inflight` returns to idle, reaper never fires.

---

## 2. Reaper — synthetic (already HW-verified; regression)

```
inflight stall        # inject a fake stale slot + kick a drain
```
**Pass:** log `reaped stale in-flight publish (id=-1) … will re-publish`; then
`inflight` reads idle; real events keep draining.

---

## 3. [GATE] Real lost/expired PUBACK on a still-associated link

The exact silent-telemetry-stop bug the branch exists to fix.

```
record_env  (×5)      # queue several real pending events
evlog                 # confirm pending ≥ 5
inflight              # confirm a REAL measure_id is latched, age climbing
# --- now block ONLY the inbound broker→device path; keep Wi-Fi associated ---
inflight              # watch age climb past 60000 ms
```
**Pass (all):**
- log `reaped stale in-flight publish (id=<REAL measure_id>)` — a real id, not -1
- event reverts to PENDING and re-publishes; **no** `drain stalled` wedge
- restore the path → PUBACK lands, `evlog` pending drains to 0, **no reboot**
- host subscriber receives the event; **confirm whether it arrives as a duplicate
  `measure_id`** and that openJII dedupes on `(device_id, measure_id)`

---

## 4. [GATE] Broker/TLS drop with Wi-Fi still up (new disconnect callback)

```
record_env            # get a real slot in flight
inflight              # confirm occupied (real measure_id)
# --- kill the broker/TLS session (block broker TCP port); keep STA associated ---
```
**Pass:** log `MQTT disconnected` → slot clears → `inflight` idle → on reconnect
(`MQTT connected`) the event re-publishes; no `drain stalled`.
**Watch the false-positive:** confirm esp-mqtt does not fire `MQTT disconnected`
transiently during its own reconnect and clear a legitimately in-flight slot.

---

## 5. Wi-Fi drop mid-publish (idempotency regression)

```
record_env
inflight              # occupied
# --- power off the AP entirely, wait, power it back on ---
```
**Pass:** log `Wi-Fi disconnected — stopping MQTT`, slot → PENDING, re-publish on
reconnect. The `mqtt_client_stop()`-induced `MQTT disconnected` running after the
Wi-Fi-drop clear must be idempotent — no crash, no double mark-pending.

---

## 6. [GATE] Connectivity watchdog + sys_evt stack (mid-TLS reboot)

The `netwd test` forces a zero-timeout eval → immediate reboot. Trigger it **with a
publish/TLS transfer in flight** to reproduce the sys_evt stack overflow the
2304→6144 B bump fixes.

```
record_env            # ensure pending > 0
netwd                 # allowed=yes, clock=yes, pending>0, would reboot now=YES
record_env            # start another publish so TLS is active…
netwd test            # …then fire the watchdog while it's in flight
```
**Pass:** log `connectivity watchdog [TEST] … rebooting`; device **reboots cleanly
with NO sys_evt / stack-overflow panic**; on boot `evlog` shows the backlog intact
(SD + NVS cursor survived) and draining resumes.

---

## 7. Reboot mid-publish (INFLIGHT → PENDING on reboot)

```
record_env
inflight              # occupied
reboot                # hard restart while the QoS1 publish is in flight
# after boot:
evlog                 # the un-acked event is still pending (cursor never advanced)
inflight              # idle
```
**Pass:** no record stuck in-flight; the event re-publishes; no data lost.

---

## 8. [NEW] `evlog rewind` — force records back to PENDING

Validates the new command. Records have no on-disk sync state — a record is PENDING
iff it's at/after the NVS read cursor — so rewind re-queues by moving the cursor.

```
evlog                 # note current cursor + a fully-drained baseline (pending 0)
record_env  (×3)      # publish 3 events; wait until evlog pending == 0
evlog rewind          # rewind to the OLDEST file on the card → re-publish everything
evlog                 # pending jumps back up; cursor at the oldest ev-*.log @ off 0
                      # log: "cursor rewound to seq=… off=0 — N record(s) pending"
```
**Pass:** the drained events re-publish; host subscriber receives them again as
**duplicate `measure_id`s** (at-least-once — must be deduped downstream);
`evlog` pending returns to 0.

Targeted form (re-publish from one file only):
```
evlog rewind 338      # rewind to ev-000338.log (clamped to files present;
                      # never advances the cursor forward)
```
Edge checks: `evlog rewind 999999` (past the tail) clamps to the current cursor and
re-sends nothing extra; `evlog rewind 0`/garbage → usage error, no change.

---

## 9. [GATE] Cert provisioning + identity end-to-end

The build's NVS hook **warns at compile time** if the client id and cert bundle
disagree — e.g. observed on this branch:
`AMBYTE_CLIENT_ID='AMBYTE_{MAC}' does not match AMBYTE_CERT_BUNDLE='399b51c5…'`.
That mismatch = AWS IoT rejects the TLS handshake.

```
# host: flash a real device_certs/<thing>/ bundle + matching .env, then on boot:
```
**Pass:** log `certs initialised (provisioned=yes)`, `MQTT client initialised
(TLS=yes)`, `MQTT connected`; device visible in the AWS IoT console; telemetry lands
under `topic_root` with the `AMBYTE_{MAC}`-form client id.
**Negative:** a client-id vs bound-thing mismatch shows connect-then-immediate
disconnect (no rc=granted) — fix by aligning `AMBYTE_CERT_BUNDLE` with the thing.

---

## 10. Healthy-link soak (guards a mis-tuned 60 s threshold)

Run over a stable link for a stretch with periodic `record_env` (or the live Lua
loop). **Pass:** one-at-a-time drain, prompt PUBACKs, `inflight` age never reaches
60 s, reaper never fires, no duplicates at the broker, watchdog never reboots.

---

## Merge blockers summary
- [ ] §3 real lost-PUBACK recovery **+ duplicate-measure_id dedup confirmed**
- [ ] §4 MQTT-disconnect clear (no false-positive on reconnect)
- [ ] §6 watchdog reboot is clean — **no sys_evt stack overflow**
- [ ] §9 provisioning connects with `AMBYTE_{MAC}` id (no handshake rejection)
- [ ] §8 `evlog rewind` re-queues + re-drains
- [ ] non-HW: commit the `tools/flash_certs.{cmd,sh}` working-tree edits; confirm a
      clean `pio run -e esp32-s3-devkitm-1`

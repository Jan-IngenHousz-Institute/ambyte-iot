# Pushing scripts to a device over MQTT

> **IMPLEMENTED 2026-06-12** (`components/script_update`, dispatched by
> `command_router`). The shipped contract, where it differs from the original
> draft below:
> - Canonical script field is **`script`**; the draft's `payload` is accepted as
>   a legacy alias. `checksum` (SHA-256 hex of the raw script) is **optional** —
>   verified when present.
> - **Inline size cap is 16 KB** for the whole MQTT message
>   (`INBOUND_MSG_LARGE_MAX`, transient heap reassembly in `mqtt_client.c`), not
>   128 KB; `_payload_encoding`/gzip is NOT implemented. A `url` variant is the
>   documented follow-up if scripts ever outgrow 16 KB.
> - Apply flow: sha256 (if given) → **Lua syntax check before the SD is
>   touched** → write `main.lua.new` + fsync → stop runner → previous script kept
>   as **`/sdcard/main.lua.bak`** → atomic rename → restart runner → NVS id
>   latch (success only, so the retained message doesn't re-apply on every
>   reconnect; a failed apply stays retryable under the same id).
> - Reply on the status topic:
>   `{"type":"script_status","device_id":…,"id":…,"state":"applied"|"failed","detail":…}`.
> - Sibling command **`lua_exec`** `{type:"lua_exec", id, code}` runs a snippet
>   immediately in an ephemeral Lua state (full device/ambit/uart/db/sync env,
>   120 s budget, alongside a running main.lua) and replies
>   `{"type":"lua_exec_result", id, ok, result}`. CLI twins: `lua
>   start|stop|status|exec <code…>`.

Deliver a Lua script to a device by publishing a small JSON message to the device's
own topic. The device subscribes with its X.509 certificate; you publish from the
AWS IoT console.

**Topic** (last segment = the Thing name):

```
device/scripts/v1/{sensorType}/{sensorVersion}/{thingName}
```

Example for `dom_ludo_prototype_ambyte_thing_v2`:

```
device/scripts/v1/Ambyte/2/dom_ludo_prototype_ambyte_thing_v2
```

**Endpoint:**

```
aws iot describe-endpoint --endpoint-type iot:Data-ATS --region eu-central-1
# e.g. a3xxxxxxxxxxxx-ats.iot.eu-central-1.amazonaws.com
```

---

## 1. Device subscribes (certificate auth)

Requirements: **clientId = Thing name**, the topic ends in the **Thing name**, QoS 1,
TLS on port 8883 with the device certificate + private key + Amazon Root CA 1.

Verify with mosquitto:

```bash
mosquitto_sub \
  --cafile AmazonRootCA1.pem \
  --cert   dom_ludo_prototype_ambyte_thing_v2.cert.pem \
  --key    dom_ludo_prototype_ambyte_thing_v2.private.key \
  -h a3xxxxxxxxxxxx-ats.iot.eu-central-1.amazonaws.com -p 8883 \
  -i dom_ludo_prototype_ambyte_thing_v2 \
  -t 'device/scripts/v1/Ambyte/2/dom_ludo_prototype_ambyte_thing_v2' \
  -q 1 -d
```

Firmware does the same via its MQTT library: clientId = Thing name, present the
cert/key/root-CA on the TLS connection, subscribe at QoS 1, and on each message verify
`checksum` then apply `payload`.

## 2. Publish the script (AWS IoT console)

IoT Core (eu-central-1) -> **MQTT test client -> Publish to a topic**:

- **Topic** (exactly the device's topic, no wildcards):
  `device/scripts/v1/Ambyte/2/dom_ludo_prototype_ambyte_thing_v2`
- **Additional configuration -> QoS 1, Retain on.**
- **Payload:**

```json
{
  "type": "script_update",
  "id": "upd-2026-06-09-001",
  "payload": "-- Lua protocol script\nreturn { pulses = 100, interval_ms = 500 }",
  "checksum": "adaeff5386f5e9dcaef58c0cc39f69f2d62a98bedae07195d02bdc82595a90ad"
}
```

- **Publish** -> the device receives it immediately.

Recompute the checksum if you change the script:

```bash
printf '%s' '<exact script text>' | shasum -a 256
```

---

## Message fields

| field | required | meaning |
|---|---|---|
| `type` | yes | always `"script_update"` |
| `id` | yes | unique update id; lets the device skip one it already applied |
| `payload` | yes | the Lua script, inline |
| `checksum` | yes | SHA-256 hex of the (decoded) `payload` |
| `_payload_encoding` | no | `"gzip+base64"` if you compressed `payload` (checksum stays over the raw script) |

## Good to know

- **Retain on** means a device that is offline gets the latest script the instant it
  reconnects and subscribes. No re-publish needed.
- **One device per publish.** For many devices, publish once per Thing name.
- **Isolation:** a device can only subscribe to its own topic (the certificate policy is
  scoped to its Thing name); it cannot read other devices' scripts.
- **Size:** keep the whole message under 128 KB (use `gzip+base64` for large scripts).
- **Not received?** The topic must match character-for-character on both sides
  (`sensorType` / `sensorVersion` / Thing name). If the subscribe is refused: clientId is
  not the Thing name, the certificate is not attached to the Thing, or the policy/topic
  Thing-name segment do not match.

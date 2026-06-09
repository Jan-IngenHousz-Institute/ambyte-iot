# Pushing scripts to a device over MQTT

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

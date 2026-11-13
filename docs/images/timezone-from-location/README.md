# Timezone from location: bench captures

Real USB console and MQTT output from the DEV bench Ambyte (28:37:2F:FF:E7:04).
It was captured on 2026-11-13 during the qualification of PR #100. Each `.txt`
holds the selected lines verbatim, with ANSI colours stripped. The `.png` next
to it renders the same text.

Non-adjacent lines are excerpts, not a continuous session. Titles and `#` lines
are annotations. **Redacted:** the coordinates of every live estimate (they
locate the bench site), and the Wi-Fi SSID, BSSIDs and public IP, which never
appear in these excerpts. Nothing was generated to stand in for missing
evidence.

| capture | firmware | shows |
| --- | --- | --- |
| `live-location` | v2.16.0-10-g67e8948 | Boot with the injective device-location id (`ambyte-5F28-3A37-3A2F-3AFF-3AE7-3A04`).<br>Both reply subscriptions granted.<br>The persisted live fix in use after the reboot (`age_s` 457).<br>`locate` → Wi-Fi estimate ±41 m.<br>`locate ip` → plain-HTTP public IP → IP estimate ±5000 m.<br>identity after each step, with the zone derived from the live location. |
| `inert-without-policy` | v2.16.0-7-g7344e3b | The device-location test policy was detached from the bench certificate (09:26:43Z–09:28:10Z).<br>Both device-location SUBSCRIBEs refused with 135 (0x87), logged once.<br>No disconnect, nothing published on the request topic, no reason-coded PUBACK.<br>identity `live.status` = `not_authorized`. |
| `solar-follows-location` | v2.16.0-10 (before) / v2.16.0-14-g19ecd20 (after) | Before: a live fix was active, but `sync sun` used the compiled NL default.<br>After: with no confirmed fix, `solar_position: default`; `locate` → the Wi-Fi fix takes over the sun position (sunrise 07:54 → 07:59).<br>A later ±5 km IP answer leaves the Wi-Fi fix and position alone. |
| `country-rule-and-status` | v2.16.0-21-gf964045 | Configured Amsterdam centre + the bench's Wi-Fi fix in the same country: the configured site keeps zone and sun position, and the live fix is informational (`live.source: wifi`, `wifi_age_s`).<br>A PUBLISHED STATUS heartbeat on the DEV data topic carries `health.location` with `solar_position`, `live_source` and `live_wifi_age_s`. |
| `mqtt-timezone` | v2.16.0-7-g7344e3b | `fleet_command.py` run locally against DEV:<br>`get-timezone`.<br>`set-timezone Asia/Tokyo`, reported as a FALLBACK because the location wins.<br>`--pin Europe/Berlin` → pinned.<br>`auto` → location.<br>Restore of the configured zone `Europe/Amsterdam`. |

The second bench slot (country rule, published STATUS) ended with the official-source 2.16.0 image restored and verified: cfg, schedule (`ba183e96…`, workbook `ce2b1235…`, wb-v11) and the configured Amsterdam-centre position were identical to the slot's baseline.

The first bench slot was restored at the end. Every configuration key that firmware 2.16.0
reads is byte-identical to the baseline, and the installed schedule
(`943c2942…`) and workbook (`75c59552…`, wb-v7) are unchanged. Two keys from
this firmware remain:

- `tz_source=auto`, which means not pinned.
- the accepted live fix.

Firmware 2.16.0 ignores both.

# MP2731 power-read bench excerpts (2026-11-13)

The terminal images show selected real console lines from the dev bench Ambyte
`28:37:2F:FF:E7:04`. It is powered over USB, so its charger is always in charge
mode with VIN present. The battery-only state of the field incident cannot be
shown on this bench; the host tests and the prod raw archive cover it (see
[../../power-telemetry.md](../../power-telemetry.md)).

Each `.txt` beside an image keeps the selected source lines, prefixed with
their original 1-based line numbers in the capture file. Non-adjacent lines are
excerpts, not one continuous session. The numbering restarts where a second
capture file begins (for example the `probe` block). The device MAC and the
experiment id are redacted in the MQTT topic. Titles, wrapping and colours are
presentation only: the firmware output is otherwise unchanged. No line was
written to stand in for missing evidence.

| Image | Firmware | What it shows |
|---|---|---|
| `v2.16.0-registers` | official-source 2.16.0 (`status`), then 2.16.0 + the read-only `mp2731_regs` dump with the old driver | Readings move in charge mode. REG03 = 0x10 (no ADC bit stuck), REG0C = 0x39 (VIN_STAT 001, charge done), REG0D = 0x80 (I2C watchdog expired: the charger runs in "default mode") |
| `fix-verified-reads` | this PR (`v2.16.0-2-g56b92f4` + the probe commit) | Every read is `ok`, START falls within the first 5 ms poll, results change between reads. The gate opens with reason `external_power`. `probe`: START already reads 0 about 0.6 ms after the start write, so the poll catches only a stuck-high bit |
| `fix-heartbeat-mqtt` | this PR | A heartbeat received over dev MQTT with `power.valid: true`, `reason: ok`, `publish_gate_reason: external_power`, while the charger moved from charged (3) to CC charging (2, 175 mA) |

The board was restored to official-source 2.16.0 afterwards (NVS preserved;
`identity` fw 2.16.0, signed).

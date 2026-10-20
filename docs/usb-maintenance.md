# USB maintenance contract v1

This is the supported maintenance subset of the physical USB console for an
already provisioned Ambyte. It works without network, SD card or an unlock.
Physical access grants authority to the existing write commands. This document
covers destination configuration and schedule installation; Wi-Fi credentials,
certificate provisioning, sensors, firmware transfer, record export, receipts
and radio authorization have separate contracts.

## Connect and find a response

Use USB-Serial/JTAG at 115200 baud. Deassert DTR and RTS **before opening** the
port; asserting them can reset the ESP32-S3. The existing Python client uses:

```python
import serial

ser = serial.Serial()
ser.port = "/dev/ttyACM0"
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()
```

After a reset, the console can take 20–35 seconds to appear. It retains the
`ambyte> ` prompt, command echo and diagnostic output. Send each command as an
ASCII line terminated by LF. The parser allows 512 bytes, but the USB driver's
RX buffer is only 256 bytes. Keep a complete request **at most 224 ASCII bytes,
including its newline**, counting the command, spaces, quotes and values.
Longer values may fit their configuration buffer but cannot safely be pasted
through this transport. Use quoted arguments for spaces; escaped quotes and
backslashes count toward the wire limit.

For discovery, split received bytes into physical LF-terminated lines, remove
only trailing CR/LF, and select the first line that parses as one JSON object
whose `contract` is an integer (a JSON boolean does not qualify). The line must
start with `{` at column 0 and have no bytes after its closing `}`. Check the
version and required fields/types before using the result. Ignore blank lines,
prompts, echoes and unrelated logs. Do not strip a prompt/log prefix to make a
damaged line appear valid. The handler emits a leading line break to separate
an unfinished prompt or log tail, then locks stdout through its whole JSON
record and flush.

Normal ESP-IDF v1 logs pass through the sd_logger console tee to the original
`vprintf` on the same newlib stdout FILE, so the stdout lock prevents those logs
from splitting the response. ROM output (`esp_rom_printf`), early/DRAM logs,
panic output and libraries that print through ROM bypass this lock. Discard
any line that does not parse, and treat a reset or incomplete response as an
unconfirmed read. This console has no correlated requests or durable result
lookup in v1.

An older image receiving `identity` answers:

```text
Unrecognized command
```

Treat that as unsupported firmware. An unknown command on the new image keeps
the same error. A recognized handler returning 1 is followed by the existing
ESP-IDF diagnostic:

```text
Command returned non-zero error code: 0x1 (ERROR)
```

The blocks below omit prompt/echo/log lines and show response text. Output
line endings may be LF or CRLF (the device's VFS translates stdout newlines).
Angle-bracketed values are placeholders for the particular request/device,
rather than literal response bytes.

## Discover the running identity

Request:

```text
identity
```

Illustrative response, on one physical line:

```json
{"mac":"02:00:00:00:00:01","thing_name":"ambyte_02:00:00:00:00:01","fw":"2.6.0-a2","contract":1,"schedule":{"source":"NONE","sha256":null,"workbookVersionId":null,"script_version":null},"capabilities":["identity","cfg.get","cfg.set","reboot","schedule.status","schedule.release","schedule.begin","schedule.put","schedule.commit","schedule.abort"]}
```

| Field | Meaning |
| --- | --- |
| `mac` | Wi-Fi station hardware MAC, uppercase and colon separated; null if unavailable. Independent of configured measurement device id. |
| `thing_name` | Effective MQTT client id resolved at boot, preserving case, after NVS/Kconfig fallback and `{MAC}` expansion. No cloud lookup. A cfg write cannot change this value until reboot. Null if unavailable. |
| `fw` | `esp_app_get_description()->version`, the compiled application version. Stored provisioning version strings are not authoritative. Null if unavailable. |
| `contract` | Integer `1`. |
| `schedule.source` | Loaded program's source: `NONE`, `INSTALLED`, `EMBEDDED_DEFAULT`, `EMBEDDED_FALLBACK`, or null while no consistent snapshot is available. |
| `schedule.sha256` | SHA-256 of the exact **loaded** YAML bytes: 64 lowercase hex characters, or null if unavailable. |
| `schedule.workbookVersionId` | Workbook from the loaded program, or null when absent/unavailable. Never an empty string or the human CLI's `-` sentinel. |
| `schedule.script_version` | Release provenance only when the loaded INSTALLED digest equals the installed-file digest and verified persisted release metadata. Null for embedded programs, stale/missing/incomplete metadata or a file being renamed/unavailable. Not the YAML header `version`. |
| `capabilities` | Unique implemented v1 tokens below; ordering has no meaning. |

The snapshot includes source, digest and workbook from one program generation.
A reload spanning the release-metadata read makes all four schedule fields
null. Before any program is loaded, source is `NONE` and the other three
fields are null. A stopped-but-loaded program retains its loaded identity;
discovery alone does not claim jobs are running. A staged or queued upload is
never reported as activated.

The v1 capability tokens are exactly:

```text
identity cfg.get cfg.set reboot schedule.status schedule.release
schedule.begin schedule.put schedule.commit schedule.abort
```

These tokens identify the existing space-separated commands; `cfg.get` is
invoked as `cfg get`, for example. No Wi-Fi, export, receipt, firmware-update,
sensor or radio capabilities are advertised. `identity unexpected` produces
`Usage: identity` and the existing nonzero diagnostic, with no identity object.

After each response has been flushed and its stdout lock released, one normal
`identity` INFO log reports `budget internal_free=<bytes>
internal_largest=<bytes> internal_min=<bytes> cli_stack_min=<bytes>` on one
log line. These are internal heap free/largest, heap since-boot low-water, and
console stack high-water margin in ESP-IDF byte units. They are diagnostics
outside the JSON contract.

## Read and set destination configuration

Only the following existing string keys are in this subset. Never put userinfo,
passwords, tokens or other credentials in broker URIs or any other value: cfg
echoes what was stored. No certificate/key/Wi-Fi-password key is part of this
subset. This restriction describes the client contract; the whole historical
physical console is not an access-control boundary.

| Key | Safe effective value length in bytes / behavior |
| --- | --- |
| `mqtt_uri` | 255; broker endpoint only, without authentication information. |
| `mqtt_client_id` | 63; becomes effective thing name after boot expansion. |
| `mqtt_topic_root` | 255. |
| `command_topic`, `status_topic` | 287 each; practical USB request limit is smaller. When absent, boot derives them from the topic root. |
| `device_id`, `device_name` | 63 each; payload identity/name, distinct from hardware MAC and thing name. |
| `protocol_id` | 31. |
| `device_version`, `device_firmware` | 15 each; provisioning metadata, never the compiled firmware version. |
| `firmware_version` | Legacy stored string, not an authoritative runtime version. The cfg read buffer permits 319 bytes, but the USB request limit still applies; use short values. |
| `timezone` | 47 after canonicalization; supported IANA zone or existing alias (for example `AMT` → `Europe/Amsterdam`). Invalid zones are rejected. |
| `publish_gzip` | `0/off/false` or `1/on/true`; getter normalizes to `0` or `1`. Existing exception: applies immediately, despite the generic reboot message. |

These are buffer-derived limits for reliable boot readback, **not new firmware
validation**. Most existing setters accept arbitrary NVS strings and do not
enforce the listed limits. Oversized strings can make reads fail, fall back to
compiled defaults or become unavailable. MAC expansion must fit the same
effective buffer; do not rely on truncation. Request size includes syntax,
so a 223-byte value is already too long for most cfg requests.

Read a stored value:

```text
cfg get mqtt_client_id
mqtt_client_id = ambyte_{MAC}
```

That is the raw stored string. It does not perform MAC substitution or show
compiled defaults. On an unset string key, the response template is:

```text
<key> = (unset: ESP_ERR_NVS_NOT_FOUND)
```

Write and read a benign name:

```text
cfg set device_name A2_Bench
cfg set device_name = A2_Bench  (reboot to apply)
cfg get device_name
device_name = A2_Bench
```

Successful cfg writes commit the stored value. Destination values are read
once at boot. Readback before reboot proves storage, not the active MQTT
configuration. `{MAC}` is expanded at boot for `mqtt_client_id`,
`mqtt_topic_root`, `device_id`, `device_name`, `command_topic` and `status_topic`.
`publish_gzip` is the immediate-apply exception; timezone get returns its
canonical form. For `firmware_version`, use `identity.fw` for the compiled
version regardless of what cfg says.

Unknown key:

```text
cfg get no_such_key
cfg: unknown key 'no_such_key'
Command returned non-zero error code: 0x1 (ERROR)
```

Wrong argument counts print `Usage: cfg | cfg get <key> | cfg set <key> <value>`
followed by `Keys:` and the existing table above, then the nonzero diagnostic.
A failed setter prints `cfg set <key> failed: <ESP error name>`, also followed
by that diagnostic. Do not treat an echo or prompt as write success.

## Reboot and verify

```text
reboot
Rebooting...
```

Wait for the console again; if it re-enumerates, reopen with DTR/RTS deasserted.
Read back the key and issue `identity`. For a changed client-id template, verify
that `thing_name` is the intended expanded value and the MAC and compiled
firmware remain the expected values. Save the original stored values before
changing them and restore them after a bench test. Do not use `wifi_reset` or
`wifi_join` to arrange an offline test: obtain disconnection from the configured
AP being off or out of range and confirm `status` reports disconnected and
provisioned. Provisioned credentials must survive.

## Stage a schedule without network or SD

Staging uses internal littlefs, `/littlefs/schedule.yaml.new`. Only one client
and installer may use it at a time. `begin` truncates any existing staged file.
`put` appends, with no offset, idempotence or resumability. If delivery of a put
is uncertain, abort and restart from the exact bytes rather than retry an append.
The total decoded YAML cap is **16,384 bytes**.

Before changing a bench schedule, save the original release tuple and obtain
the exact original YAML bytes (for example from the matching release asset or
stamped experiment source). Their SHA must match the currently loaded INSTALLED
SHA in `schedule status` and installed-file SHA in `schedule release`. Keep its
workbook, script version and built-against firmware. If those restore bytes or
provenance cannot be obtained, stop before uploading. The console has no
download/restore-old-schedule command in this subset.

Compute SHA-256 over the exact bytes to send, including final newline and all
comments/whitespace. Send **144 raw bytes** per base64 chunk (192 characters;
`schedule put ` makes the normal line 206 bytes including LF). No whitespace
inside the base64 token. The final chunk may be shorter.

```text
schedule begin
schedule begin: ready
schedule put <base64-chunk>
schedule put: <cumulative decoded bytes> bytes
schedule validate
schedule.yaml.new: valid
schedule commit <sha256> <id> <script-version> <built-against-fw>
schedule commit queued: id=<id>
```

`schedule validate` is the existing optional compile check before commit. On
failure it prints `schedule.yaml.new: <compile reason>` and the nonzero
diagnostic; the active program is untouched. It has no v1 capability token.

Commit requires 64 hexadecimal SHA digits. Use a fresh whitespace-free id
of at most 63 bytes, and script version / built-against firmware of at most
31 bytes each. The **whole commit line still must fit 224 bytes including LF**;
enforce the individual limits too. These existing bounded buffers truncate
longer metadata, so a client must enforce these limits.
An id is the installer's existing dedupe label; it is not a transaction id.

`queued` means only that a worker accepted the request. The worker stops the
runner, hashes/compiles the staged bytes, preserves the previous file as
`schedule.yaml.bak`, swaps, persists provenance and restarts the runner.
Do not issue another begin/put/abort/install while the queued worker might
still own staging. No operation bound or durable result lookup is promised
by v1; HAR-163 owns correlated operations, bounds, resumption and durable results.

Discard staging before a commit is queued:

```text
schedule abort
schedule abort: discarded
```

Abort is idempotent cleanup of staged bytes. It does **not** cancel an already
queued or in-flight install. Active schedule identity stays unchanged.

## Verify activation and recognize failure

After queued acceptance, read all three views:

```text
schedule release
schedule release: sha256=<installed-file-sha> version=<release-version-or-dash> built_against_fw=<fw-or-dash> installed_on_fw=<fw-or-dash> verified=<true-or-false> running=<true-or-false>
schedule status
schedule: RUNNING  source=INSTALLED
  sha256=<loaded-sha>
  id=<schedule-id> version=<header-version> workbook=<workbook-or-dash> name=<name>
identity
```

Status then prints the existing per-job table (including `runs`, failures and
next due times). A stopped runner prints `schedule: stopped  source=<source>`
but can retain its loaded program. Release reports the **installed file's**
digest; status and identity report the **loaded program's** digest. A matching
release file alone or `running=true` alone cannot prove activation.

Activation succeeds only when all of the following agree:

- Runner is running, and loaded source is `INSTALLED`.
- Loaded SHA from identity/status and file SHA from release equal the intended
  exact bytes' SHA.
- Loaded workbook equals the intended workbook (or explicit null if absent).
- Release says `verified=true`; its version and built-against firmware equal
  the intended provenance, and identity's `script_version` equals that version.

`installed_on_fw` is historical: an image change does not invalidate verified
metadata when the persisted SHA still matches the installed file. It records
the compiled firmware that installed that schedule, rather than today's image.
Missing/incomplete/stale metadata prints dashes/`verified=false` in release
and null for identity's script version. Do not infer release version from the
header's schema, header version or workbook.

A timeout, stopped runner, incomplete metadata, queued-only response or
unavailable snapshot is not proven activation. Keep the outcome pending or
unconfirmed until the views agree. Embedded fallback is explicit:
`EMBEDDED_FALLBACK` in identity/status names the embedded program's SHA and
workbook with null script version, and status gives its fallback reason.
`EMBEDDED_DEFAULT` means no installed file. Neither is successful activation
of the requested installed bytes.

Negative staging responses to recognize:

| Request / condition | Existing response text (then nonzero diagnostic for synchronous failures) |
| --- | --- |
| `schedule put !!!` | `schedule put: invalid base64` |
| Put without staging | ``schedule put: no staged file, run `schedule begin` first`` |
| Decoded cap exceeded | `schedule put: refusing to exceed 16384 bytes` |
| `schedule commit bad id version fw` | `schedule commit requires a 64-digit SHA-256` |
| Commit with 64 nonhex characters | `schedule commit SHA-256 must be hexadecimal` |
| Worker cannot queue commit | `schedule commit queue failed: <ESP error name>` |
| Staging cannot be created | `schedule begin: cannot open /littlefs/schedule.yaml.new` |

For a valid-checksum but invalid YAML file, commit can still answer queued.
The worker logs `schedule compile failed: <reason>, schedule.yaml untouched`
and resumes the old program. A mismatched checksum logs
`checksum mismatch, script rejected`. These are asynchronous diagnostics,
not successful commit responses. Recheck loaded SHA/workbook/release; they
must still describe the previous program. Do not abort/reuse staging until
the worker has finished. If fallback occurs, inspect status and do not claim
that the intended bytes activated.

For bench qualification use an explicit test workbook UUID and a fixture that
cannot capture during the test window; confirm its job table still has
`runs=0` before restoration. If it captures anything, record the event ids
because reconnecting to dev MQTT may publish their test provenance. Restore
the original schedule using the saved exact bytes/provenance, then verify its
original SHA, workbook and script version, and restore changed cfg values.
No queue/cursor edits, replay or record deletion are part of this workflow.

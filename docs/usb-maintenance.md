# USB maintenance: console v1 and framed service v1

This is the supported maintenance subset of the physical USB console for an
already provisioned Ambyte. It works without network, SD card or an unlock.
Physical access grants authority to the existing write commands. This document
covers the existing console subset and the explicit framed `svc` mode for
durable schedule transactions and bounded Wi-Fi credential writes. Certificate
provisioning, sensors, firmware transfer, export, receipts and radio authorization
are outside this contract. The console and service each have version `1`;
entering service does not change the plain commands described first.

## Connect and find a response

Use USB-Serial/JTAG at 115200 baud. Deassert DTR and RTS **before opening** the
port, as the existing Python client does. **Opening may still reset the gateway**
on the host/driver in use, even with both lines deasserted. Always wait for the
REPL after every open before sending discovery or another command. A command
sent during boot can be lost; silence before a prompt is not an unsupported
firmware response. The existing Python client opens the port as follows:

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

After an open/reset, the console can take 20–35 seconds to appear. Wait until
the `ambyte> ` prompt is observed; keep the same port open through discovery
and later operations, since reopening may reset it again. The console retains
command echo and diagnostic output. Send each command as an
ASCII line terminated by LF. The parser allows 512 bytes, but the USB driver's
RX buffer is only 256 bytes. Keep a complete request **at most 224 ASCII bytes,
including its newline**, counting the command, spaces, quotes and values.
Longer values may fit their configuration buffer but cannot safely be pasted
through this transport. Use quoted arguments for spaces; escaped quotes and
backslashes count toward the wire limit.

For discovery, split received bytes into physical LF-terminated lines, remove
**all** trailing CR/LF bytes (for example `line.rstrip(b"\r\n")`, not just one CR),
and select the first line that parses as one JSON object
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
unconfirmed read. This plain-command subset has no correlated requests or
durable result lookup; the framed service below supplies those separately.

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

The blocks below omit prompt/echo/log lines and show response text. Accept LF,
CRLF and CR-CR-LF line endings. On the current USB build, `identity` and many
legacy handlers end their text with `\r\n`, and the VFS converts that LF to
CRLF again: the JSON response therefore ends with **`}\r\r\n`**. IDF diagnostics
that write just LF become CRLF. Strip the whole trailing CR/LF sequence before
checking a line, while still rejecting any other bytes after the closing `}`.
Angle-bracketed values are placeholders for the particular request/device,
rather than literal response bytes.

## Discover the running identity

Request:

```text
identity
```

Illustrative response, on one physical line:

```json
{"mac":"02:00:00:00:00:01","thing_name":"ambyte_02:00:00:00:00:01","fw":"2.6.0-a3","contract":1,"schedule":{"source":"NONE","sha256":null,"workbookVersionId":null,"script_version":null},"capabilities":["identity","cfg.get","cfg.set","reboot","schedule.status","schedule.release","schedule.begin","schedule.put","schedule.commit","schedule.abort","svc"]}
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

Discovery strings must be valid UTF-8. If a stored or injected string contains
any invalid UTF-8 sequence (including a truncated sequence, overlong encoding,
surrogate or value above U+10FFFF), the **entire field is JSON null**. Valid
UTF-8 is preserved exactly, with JSON escaping for quotes, backslashes and
control characters. This also applies to thing name, compiled version, workbook
and release version: discovery never invents a replacement identifier or emits
invalid JSON. A null identity/provenance value cannot count as a match to an
expected identifier. This read does not change the stored bytes.

The existing v1 capability tokens are:

```text
identity cfg.get cfg.set reboot schedule.status schedule.release
schedule.begin schedule.put schedule.commit schedule.abort
```

These tokens identify the existing space-separated commands; `cfg.get` is
invoked as `cfg get`, for example. A3 adds only the additive `svc` token;
its implemented actions are discovered through `svc.version`. A2 has no `svc`
token. No export, receipt, firmware-update, sensor or radio capabilities are
advertised. `identity unexpected` produces
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
by these plain commands; use the framed service below for correlated operations,
bounded admission, resumption and durable results.

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

## Enter service and validate every physical line

Use the same open USB port and the DTR/RTS rule above. An `identity` capability
`svc` advertises this mode. Enter with the plain command `svc` and no arguments.
Wrong arguments print `Usage: svc` and the existing handler error. On entry,
ordinary echo/prompt/log output ends before this framed ready response:

```json
{"ok":true,"store":"0123456789abcdef","service":1,"state":"ready"}
```

That JSON is the body of correlation-zero frame, not a bare console response.
Its store is the actual device instance, or JSON null if unavailable. From ready
through the exit response, normal USB writes from all tasks are suppressed,
including ESP_LOG, printf/puts, stdout/stderr and direct driver writes. Existing
SD log tee output continues. Dropped USB bytes/calls are counted, with no log
buffer to replay. A reset, ROM output or panic invalidates the session; an
unframed byte is never stripped away to manufacture a valid frame.

Every request and response is exactly:

```text
<8 lowercase hex correlation><space><body>*<8 lowercase hex CRC-32><LF>
```

CRC is IEEE/zlib CRC-32 over the bytes from the first correlation digit through
the last body byte, including the space, excluding `*`, CRC and LF. Requests
are printable ASCII. Responses contain one UTF-8 JSON object. Verify framing
and CRC **before** parsing JSON; reject duplicate keys, wrong field types,
nonfinite JSON values, trailing bytes and correlation mismatch. Never salvage
a response by removing a prompt or log prefix. CRLF is invalid in service.

| Bound | Service v1 value |
| --- | --- |
| Request | 248 bytes total, including LF; 144 raw bytes in a full `p` chunk |
| Response | 1,024 JSON bytes, 1,043 wire bytes including LF |
| Requests outstanding | One; stop-and-wait is required |
| Partial line | 2 seconds from its first byte, then `frame_timeout` and drain |
| Session idle | 60 seconds without input, then `session_timeout` and REPL exit |
| USB response emission | At most 1 second; a failed write does not undo a stored result |
| Client response wait | 5 seconds; timeout means unknown delivery/outcome |

Nonzero correlation IDs identify exchanges only; they are not durable operation
IDs. Correlation zero is reserved for ready/framing/session notices. A valid
unsupported action returns a correlated `unsupported` error. Invalid/oversized
input drains to LF. The fixed 256-byte RX ring can drop bytes if a client
pipelines: a detected overflow produces `rx_overrun`, invalidates binding and
drains to a quiet boundary. It cannot return a response for each entirely lost
request. Reprobe, rebind and reconcile every unconfirmed operation; no spliced
suffix is executed. Do not increase the ring or infer success from silence.

`svc.exit` returns its correlated exit frame, then restores the ordinary REPL.
Disconnect does not immediately exit mode, and active operations/results remain.
For up to 60 seconds, ordinary flash_gui/fleet console parsers can receive framed
errors and fail to find a prompt. Wait for idle exit or issue a valid framed
exit. The GUI schedule installer recognizes a valid framed notice, performs a
fresh read-only service probe/bind/exit, then resumes typed identity discovery.
Repeated plain nudges count as input and can keep service mode alive.

Outside service, a pre-editor filter withholds eight lowercase hex digits and
the following space, drains framed input and answers `mode_required` without
echoing it or adding history. Withheld bytes are released as soon as a byte
rules out that prefix, never merely on idle. A human typing a pure-hex prefix
therefore sees its echo late; ordinary commands and terminal escape/probe
sequences pass through unchanged. Older images lack this protection: only a
nonsecret version probe may be sent before fresh mode confirmation.

## Discover, bind and identify a durable operation

All service responses carry boolean `ok` and `store` (16 lowercase hex digits,
or null if unavailable). Errors add only a fixed nonsecret `reason`. Read
`svc.version` before trusting capabilities or sending a write:

```json
{"ok":true,"store":"0123456789abcdef","service":1,"actions":["svc.version","svc.bind","svc.result","svc.stats","schedule.begin","p","schedule.commit","svc.abort","svc.wifi.join","svc.wifi.status","svc.exit"],"max_line":248,"chunk":144,"result_slots":8,"highest_id":"0000000000000000"}
```

`service`, limits and slot count are integers; actions are unique strings.
Production advertises exactly those actions. `svc.bind <store>` must then
confirm `{"ok":true,"store":"…","state":"bound"}` for the discovered store.
Every new connection requires a new probe/bind. A wrong binding clears the old
binding and returns `store_changed`. Version, diagnostics and exit are usable
without binding; mutations and result lookups require it.

The nonzero store instance is created randomly and persisted when a fresh
service store initializes. A changed instance voids **all earlier IDs and
outcomes**. Existing mount autoformat policy is unchanged: A3 cannot promise
survival across a whole littlefs reformat. A fresh store gets a new instance;
never automatically replay an old request into it.

Choose a durable operation ID strictly greater than `highest_id`, encoded as
16 lowercase hex digits, nonzero, without wrapping. Persist `(gateway MAC,
store, op, immutable descriptor)` on the client before admission. Use stable
hardware MAC rather than the OS port name. Concurrent clients can race: resolve
busy/conflict by readback, without taking over another client's operation.

The same retained ID and identical descriptor returns current state without
executing again. Changed size/digest/release/workbook, action kind or credentials
returns `request_conflict`. The device stores hidden, domain-separated,
length-delimited fingerprints; passwords and fingerprints are not returned.
Missing ID above the watermark is `not_found`; missing ID at/below it is
`expired`. **Expired means outcome unknown, verify device state.** It includes
reservation-crash gaps and IDs never admitted; it proves neither success nor
failure. Expired IDs cannot execute again. At the maximum ID, new admission is
`id_exhausted`; retained results and identical retries remain readable.

One nonterminal operation is admitted across schedule and Wi-Fi. A different
operation gets `busy` before side effects. Results, matching retries and a
permitted abort remain usable while busy. The existing maintenance worker
executes admitted jobs; service adds no task, queue or task-priority change.

## Service actions and result schemas

Sizes/offsets are exactly eight lowercase hex digits; digests are 64 lowercase
hex SHA-256 digits. Release tokens are 1..31 ASCII bytes from
`[A-Za-z0-9_.+-]`. Workbook is a lowercase canonical UUID or `-` for absent.
Base64 is canonical, padded, without whitespace.

| Request body | Admission, effect and response fields after `ok,store` |
| --- | --- |
| `schedule.begin <op> <size> <sha256> <version> <built_fw> <workbook>` | 1..16,384 exact YAML bytes. Persists descriptor/private empty staging before ACK. Reply `op,state,offset`, initially `staging,0`; identical replay returns existing state/offset. Never truncates legacy `.new`. |
| `p <op> <offset> <chunk_crc> <base64>` | 1..144 decoded bytes; separate eight-hex CRC over decoded bytes. Reply `op,state:"staging",offset` only after data fsync and durable offset update. Full request is exactly 248 bytes. |
| `schedule.commit <op>` | Complete prefix required. Persists `committing` before queueing; replies with the full result schema below. Hash, compile and declared/header workbook equality are checked before installation. Both absent is a match. Checks actual loaded source/digest/workbook/running plus verified release version/built_fw after activation. |
| `svc.abort <op>` | Own staging only: durably records `aborted` before private data cleanup. Reply `op,state:"aborted",reason:"aborted"`. Repeated abort reads that record. Cannot cancel admitted commit or another client's work. |
| `svc.result <op>` | Full nonsecret retained descriptor, state and evidence. Never retries the action. |
| `svc.wifi.join <op> <ssid_base64> <password_base64-or-dash>` | Distinct bounded credential write. Immediate `connecting` result after durable admission; outcome below. SSID 1..31 UTF-8 bytes, no NUL. Password empty (`-`) or 8..63 printable ASCII bytes. No truncation, enterprise credentials or certificate inputs. |
| `svc.wifi.status` | `configured_ssid,trial_ssid,trial,connected,provisioned`; current RAM/connection read, not NVS proof. Original/configured SSID remains separate from the trial SSID (`null` outside trial). A current IP can belong to the trial while `trial:true`. |
| `svc.stats` | Integer `free_heap,largest_block,min_heap,min_free_heap,min_largest_block,samples,cli_stack,worker_stack,runner_stack,dropped_bytes,dropped_calls,logical_bytes,records,log_buffer_bytes,fs_total,fs_used`. Byte units except counts; log buffer is zero. `min_heap` is the since-boot allocator low-water; `min_free_heap` and `min_largest_block` are sampled minima since service initialization. Samples cover parser allocation/free and compile boundaries, schedule validation buffers, operation boundaries and each 250 ms in mode. Runner stack is cached from the runner itself across task generations; zero means no sample yet. `fs_used` is total littlefs allocation, so measure its delta with other filesystem writers quiet; it is not per-directory attribution. Heap/stack diagnostics do not themselves establish a qualified resource floor. |
| `svc.exit` | `state:"exit"`, then prompt restoration. Leaves staging/results intact. |

A schedule result has exactly the base fields below, plus optional boolean
`timed_out` when the commit progress deadline was exceeded:

```json
{"ok":true,"store":"0123456789abcdef","op":"0000000000000001","kind":"schedule","state":"succeeded","reason":"","offset":16384,"size":16384,"sha256":"<exact-byte-sha256>","workbook":null,"version":"a3","built_fw":"test","applied":true}
```

`applied:true` means this operation installed it. Already-active exact tuple
returns `applied:false,reason:"already_active"` without reinstall. Reboot
reconstruction can return `applied:null` when attribution is unprovable.
The descriptor is size, expected SHA, workbook (null if absent), version and
built_fw; a client compares every field before resuming or accepting a result.

A Wi-Fi result replaces the schedule-specific fields with:

```json
{"ok":true,"store":"0123456789abcdef","op":"0000000000000002","kind":"wifi","state":"failed","reason":"wifi_auth_rejected","offset":0,"size":0,"ssid":"<attempted-SSID>","saved":false,"connected":false,"retrying":true,"restored":true,"restoration_connected":true}
```

`saved` and requested-trial `connected` are boolean or null when unprovable.
`retrying,restored,restoration_connected` are booleans. Restoration connectivity
is separate from success of the attempted new credentials. A historical result
does not assert the device is still connected or still runs that program now.

| Existing schedule state | `p` | `schedule.commit` | `svc.abort` |
| --- | --- | --- | --- |
| staging | Validate/write/replay | Require complete bytes, admit commit | Persist aborted |
| committing | commit_started | Read same current result | commit_started |
| succeeded/failed | operation_complete | Read same terminal result | operation_complete |
| aborted | operation_complete | operation_complete | Read same aborted result |

Wrong action kind on a nonterminal Wi-Fi operation is `invalid_sequence`.
Matching terminal commit/result reads never start another worker. Unknown IDs
retain `not_found/expired` semantics rather than masquerading as terminal ones.

## Durable chunk and commit boundaries

At the current durable offset, a valid chunk appends; data is flushed/fsync'd
before the record offset is replaced and before ACK. Earlier complete aligned
chunks compare exact stored bytes/CRC: identical replay returns the **current**
offset without another append. Changed bytes, overlap or non-boundary offsets
are `offset_conflict`; a forward offset is `offset_gap`. A short chunk is allowed
only at the declared end. CRC, length and offset errors leave the prefix intact.

The service store is private `/littlefs/svc/`: a 32-byte `SVCS0001` header with
instance/watermark, 512-byte `SVCR0001` CRC-protected records, `stage.bin`, and
bounded atomic `write.tmp` workspace. It persists no raw pointers or compiler
struct padding. Logical service files are bounded to 32 KiB: one active payload
at most 16 KiB, the last eight terminal results, header and replacement/GC space.
Actual littlefs allocation is larger and is qualified separately. Admission
requires 64 KiB free beyond proposed payload plus 2,048 bytes metadata/workspace.
No capture path, partition, MQTT payload field or new NVS namespace/key is added.

The watermark is committed before admission; a crash in that gap consumes the
ID but executes nothing. Record replacement checks write/flush/fsync/close and
rename. Uncommitted temp data never substitutes for committed evidence. After
reboot, an unacknowledged stage tail is truncated to the recorded offset;
missing/corrupt acknowledged bytes fail closed. A corrupt/unsupported service
store preserves files and refuses new writes, without resetting its watermark.

Eight newer terminal completions expire an older terminal result. There is no
wall-clock TTL. Active staging does not expire on disconnect/reboot; it stays
reserved until explicit abort or commit. A GC/result-write failure does not justify rerunning
an install or credential write: read by ID after recovery. The worker retains
observed nonsecret completion evidence while retrying a failed result write.

Commit and join are asynchronous, so the service loop continues reading while
the maintenance worker runs. Acceptance, a queue ACK or an application log is
not completion. At 120 seconds, a still-owned schedule install remains
**nonterminal committing**, with `timed_out:true,reason:"activation_unverified"`.
Admission stays busy; there is no second worker. A later completion is recorded
once ownership ends. The client may stop polling while keeping its intent.

After reboot, staging resumes. A prior committing schedule is reconciled only
after runner startup: the complete loaded/file/release tuple yields succeeded,
with `applied:null` if attribution is unknown; otherwise uncertain. It is never
automatically reinstalled. Current success evidence requires the activation
conditions in the plain-console verification section above. Independently read
identity/release after service exit; a fallback or stopped runner cannot pass.

## Wi-Fi trial, persistence and credential handling

The requested station configuration is first held in RAM. The previous config
is snapshotted, driver storage switches to RAM, and the new credentials are
tried under an exclusive service lease. Only GOT_IP owned by this request can
permit saving. Association, a late old GOT_IP or configuration acceptance is
insufficient. Open networks and personal PSK are supported; nonempty passwords
use the existing WPA2 minimum/PMF policy. No enterprise or scan contract is added.

Before persist intent, auth rejection, timeout, driver/config error or
supersession leaves both persisted credential stores unchanged. The station is
restored to its prior RAM config and reconnects automatically, without another
join from the client. Result is failed with `saved:false`; actual restoration
and retry status are separate fields. A reboot during this trial boots on old
stored credentials and later reports uncertain/interrupted, `saved:false` and
`connected:null`.

After owned GOT_IP, the service first commits a **persist-intent marker** anchored
to previous/requested config fingerprints, then selects FLASH storage and saves
the successful config. The wifi_prov/wifi_creds namespaces are untouched.
Littlefs and esp_wifi NVS cannot commit atomically. After a reboot in this
window, the boot config fingerprint is compared with requested and previous
fingerprints: requested means saved:true, previous saved:false, neither or an
unreadable config saved:null/unprovable. The outcome remains uncertain; never
assert pre-reboot connectivity. An interrupted marker write is a pre-intent
failure because FLASH persistence was not yet allowed.

Normal success requires saved:true and observed owned GOT_IP. A save/result
ambiguity is uncertain, rather than a failure claiming old config preservation.
Retrying the same ID reads evidence; only a deliberate new ID starts a new trial.
`svc.wifi.status` is RAM readback, not proof of persisted credentials. A bench
failed-trial check must reboot and demonstrate automatic original GOT_IP/MQTT
reconnect (and the configured fleet ping) to prove boot persistence.

The trial/restoration can interrupt the link for up to 30 seconds. The existing
global maintenance lock vetoes nightly, conn-health, memory and no-PUBACK
self-reboots throughout that interval and is released on every exit. Counters
and anti-loop latches are not reset. Valid IoT Jobs firmware-update admission
during a trial stays QUEUED through the existing retry path; legacy OTA/script
commands keep their existing dropped/busy reports. Unsupported Jobs operations
remain REJECTED as unsupported; no script-Jobs support is introduced.

Credentials may be sent only after a valid service version and bind response
on the **same open port within one second**, with the expected store instance.
Reprobe immediately before join. Plain output, CRC failure, timeout/exit/reset
or reopening invalidates this permission. Once a join might have been sent,
recover by ID only: never retransmit the password automatically. On older images
a credential-bearing framed line could be echoed by the plain editor.

Password and base64 password are absent from device output, results, history
and client journals/progress/errors. Request/decode/work buffers and borrowed
USB RX ring items are wiped before reuse/release; only intended driver credential
configuration and a successful explicit persistent write retain credentials.
Do not record outgoing join frames in a bench transcript. Bench joins use only
the device's configured SSID: synthetic wrong password for the negative case,
and its own privately available credentials for the positive case. No test AP
or manual restore join is part of qualification.

## Error reasons and recovery

Reasons are fixed strings, never raw driver output, credentials or a success
substitute. Success normally has empty reason; already-active and explicit
abort have the documented nonempty reasons. Every shipped reason is listed:

| Reasons | Meaning / client response |
| --- | --- |
| bad_frame, bad_crc, line_too_long, frame_timeout | Discard rejected line; no action. Drain/reprobe if framing is uncertain. |
| session_timeout, mode_required, rx_overrun | Session/binding unavailable. Fresh probe/bind; reconcile all unconfirmed IDs. |
| invalid_args, unsupported | Invalid grammar/metadata or absent action. Do not silently downgrade a service write to legacy mutation. |
| store_changed | Binding/instance differs. Preserve old client evidence; verify current device state. |
| not_found, expired, id_exhausted | ID lookup/admission semantics above; expired is unknown, exhausted cannot wrap. |
| request_conflict, busy | Descriptor differs or another operation/worker owns admission. Read nonsecret kind/state; never take over foreign work. |
| no_space, storage_error, corrupt_store | Durable boundary was not confirmed. Read retained ID; preserve files, do not clear a store to make room. |
| chunk_crc, offset_conflict, offset_gap, size_limit, incomplete | Chunk integrity/sequence/bound or missing declared bytes. Resume only from a verified result offset. |
| checksum_mismatch, compile_failed, workbook_mismatch | Exact digest/YAML/header mismatch rejected before install side effects. |
| activation_unverified, interrupted, unprovable | Activation/reboot/save attribution is unconfirmed. State determines pending versus uncertain. Never turn a matching current program alone into historical operation success. |
| aborted, commit_started, operation_complete, invalid_sequence | Explicit abort, non-cancellable commit, terminal-state or wrong-kind sequencing. Follow the state table. |
| already_active | Succeeded with applied:false; complete intended tuple was already active. |
| wifi_auth_rejected, wifi_timeout, wifi_failed | Observed trial rejection, bounded wait expiration or other connection failure. Read saved/restoration/retry evidence. |
| wifi_not_retrying, wifi_driver_unresolved, wifi_invalid_credentials, wifi_unavailable | Retry/driver/config validity or manager readiness prevented a conclusive trial. Never infer saved/connected from the reason alone. |

For `corrupt_store`, preserve the error, store/ID information and nonsecret
transcript. Exit service (framed `svc.exit` works without a binding) or wait for
idle, then inspect plain identity, schedule status/release and configuration.
Legacy schedule commands and `wifi_join` remain available as documented bench
fallbacks; legacy join keeps its historical echo/persistence risks and is not
used to restore a failed service trial. Use a reviewed online OTA path, or an
authorized NVS-skip bench firmware path, while preserving service files for
diagnosis. Never automatically delete `/svc`, reset the watermark, erase NVS or
reformat a capture store. An explicitly authorized deliberate service reset
preserves its old files and creates a new store identity; old outcomes are void.

A compatible older A2 image ignores `/littlefs/svc/`; its legacy staging uses
`.new`, never `svc/stage.bin`. Downgrade/reupgrade qualification preserves
completed results and a staged prefix, configuration and active schedule, using
NVS-skip and recording both image digests. An unknown future service format is
refused rather than interpreted as a fresh store.

## Clients and exact framing examples

The production Python client is `flash_gui/service_client.py`, shared by the GUI
and bench adapters. It validates version/actions/schema/CRC/correlation, bounds
input carry, uses stop-and-wait and records nonsecret intent with atomic
flush/fsync/replace before admission. A MAC-specific OS lease prevents two
processes from replacing that intent. Reopening has a new port generation.
Schedule resume compares the entire returned descriptor and durable offset;
missing/expired/changed-store results never trigger automatic new-ID replay.

The GUI selects service when identity advertises it, with at most two automatic
reconnections within its monotonic six-minute transfer/polling budget. It never
falls back to console mutation after a service error. A URL-only source must
first be downloaded and digest-verified on the PC. Images without svc keep the
existing serial push and older immutable-URL fallback. Post-exit verification
uses identity → release → identity for a stable loaded source/digest/workbook/
version, with text parsed only for supplemental release fields. Disagreement
or a null snapshot cannot pass. The GUI has no live Wi-Fi join control; NVS
onboarding credential provisioning is unchanged.

Closing the app or disconnecting preserves intent. Cancel explicitly aborts
only its own verified staging operation; after commit it stops waiting and
keeps the journal. Another client's active kind/state reports busy. For an
expired/uncertain/changed-store outcome, the GUI's **New schedule request…** first
reads and shows current identity, then lets the operator preserve the previous
intent and start a new operation. Normal retry resumes the previous ID. This
does not retroactively turn an unknown old outcome into success.

These are exact LF-terminated codec fixtures with injected store
`0123456789abcdef`; they illustrate bytes, not a batch to run on an active
device. The `abc` staging descriptor is not a valid schedule to commit.

```text
00000001 svc.version*e97c2d53
00000001 {"ok":true,"store":"0123456789abcdef","service":1,"actions":["svc.version","svc.bind","svc.result","svc.stats","schedule.begin","p","schedule.commit","svc.abort","svc.wifi.join","svc.wifi.status","svc.exit"],"max_line":248,"chunk":144,"result_slots":8,"highest_id":"0000000000000000"}*bdef35cc
00000008 svc.bind 0123456789abcdef*b9f19b5f
00000008 {"ok":true,"store":"0123456789abcdef","state":"bound"}*10bdf71d
00000005 p 0000000000000001 00000000 352441c2 YWJj*5ca3bfdd
00000005 {"ok":true,"store":"0123456789abcdef","op":"0000000000000001","state":"staging","offset":3}*7ed9e192
00000009 svc.result 0000000000000001*ed2ef9eb
00000009 {"ok":true,"store":"0123456789abcdef","op":"0000000000000001","kind":"schedule","state":"aborted","reason":"aborted","offset":3,"size":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","workbook":null,"version":"a3","built_fw":"test","applied":false}*69341aa2
00000003 svc.exit*b4c015f2
00000003 {"ok":true,"store":"0123456789abcdef","state":"exit"}*5cc36d7d
```

## Bench-only throughput measurements

`pio run -e svc-bench` enables `CONFIG_AMBYTE_SVC_BENCH`; the production build
leaves it off. CI's release guard rejects benchmark symbols, command strings
and effective config. The benchmark config matches production apart from this
flag, including its dedicated 1,792-byte coredump stack. Record separate image
digests/resource evidence; never ship the benchmark image.

After binding, `bench.begin rx|tx <size8hex> <chunk8hex>` owns volatile state up
to 1 MiB, refusing a live durable operation. Its ingress uses the exact `p`
wire shape with reserved **op zero**, invalid for durable operations. Raw bytes
follow `(absolute_offset * 73 + 19) & 255`. The sink checks pattern, per-chunk
CRC, offset/length and rolling CRC. Egress `bench.read <offset8hex> <count8hex>`
returns `ok,store,offset,crc32,data`, in that order; offset/CRC are quoted eight
hex digits, data is base64. Its raw chunks are 144 or 702, with final short only.
702 gives 1,023 JSON/1,042 wire bytes; 703 exceeds the JSON bound and is refused.
`bench.end` requires full length and returns total count/CRC. `bench.abort`, end,
unbind, exit or a new session wipes diagnostic state. No payload/result is
stored, no watermark is advanced and no benchmark reconnect/resume is claimed.

The Evaluator runs the following against the appropriate already-flashed image;
these commands do not flash. Original schedule restore material and the normal
bench prerequisites still apply. The stage fixture is valid log-only YAML,
padded by comments to 16,384 bytes and explicitly aborted before any commit:

```sh
python3 tools/bench_svc.py stage --port /dev/ttyACM0 --expect-mac 28:37:2F:FF:E7:04 --image .pio/build/esp32-s3-devkitm-1/firmware.bin --yaml tests/identity_host/bench.yaml --version a3-throughput --built-fw test --workbook 11111111-2222-4333-8444-555555555555 --output /tmp/a3-stage.json
python3 tools/bench_svc.py transport --port /dev/ttyACM0 --expect-mac 28:37:2F:FF:E7:04 --image .pio/build/svc-bench/firmware.bin --output /tmp/a3-transport.json
python3 tools/bench_svc.py project /tmp/a3-stage.json /tmp/a3-transport.json --output /tmp/a3-projections.json
```

Transport runs 1,048,576 bytes ingress144, egress144 and egress702. Reports give
elapsed seconds, wire bytes in both directions, payload bytes/s, frame count,
median and nearest-rank-p95 RTT, verified pattern SHA and device CRC/count,
resource snapshots, MAC/version/UTC and image digest. Transport timing includes
begin/data/end, excluding discovery/resource reads; stage timing includes
admission/readback/p ACKs, excluding discovery/abort/exit. A resumed partial
prefix cannot be labelled a fresh 16 KiB rate. Stage cleanup is independently
confirmed before the tool reports success.

Projections are linear `payload_bytes / measured_rate` for 9,830,400-byte
backlog and 2,764,800-byte image. Transport rates exclude flash, SD, OTA writes
and export generation; production stage rates include small-file/record fsync,
which does not predict an OTA slot writer or SD export adapter. A7/A9 must
remeasure their adapters. Host-pipe correctness tests provide no device rate.
Numerical device throughput and resource floors remain bench evidence;
acquisition jitter is **not measured** by this implementation.

Service storage workspace has a 21,536-byte logical worst case: 16,384 staged
bytes + nine 512-byte records + 32-byte watermark + 512-byte atomic temp. The
physical littlefs allocation may be larger; record `fs_used` before admission,
at the largest staged prefix, during replacement/GC and after cleanup, with
other writers quiet. Do not infer flash allocation from logical file lengths.
Target builds emit `.su` stack-usage records for service handlers and USB hooks;
these are static per-function frames, while task high-water margins remain the
runtime acceptance measurement. Acquisition jitter is not measured.

Record build-side F6 evidence for each image with:

```sh
python3 tools/qualify_svc_resources.py .pio/build/esp32-s3-devkitm-1 --output /tmp/svc-release-resources.json
python3 tools/qualify_svc_resources.py .pio/build/svc-bench --output /tmp/svc-bench-resources.json
```

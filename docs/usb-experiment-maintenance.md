# Change an existing Ambyte's experiment and workbook

Open **Change experiment / workbook…** from the Device section. The window first
reads the selected USB device's MAC, client identity, compiled firmware, broker,
current experiment and installed workbook. Reading needs no sign-in or release.
It reads public configuration only, never NVS images, certificates or private keys.

To change experiment, sign in and select the destination in the main window,
then click **Change experiment / workbook…** inside the destination window.
The action requires service-capable firmware (v2.9.0+), access to the destination
experiment and its pinned workbook/programming cell, and, since HAR-345, an
openJII account that can onboard the device (see below). The device change
itself makes no register, issue or rotate request, and the existing wildcard
ingest policy permits the topic change independently of registry bindings.

## openJII bindings follow the change

The device change moves the Ambyte's data, because the experiment ID is part of
the topic stored on the unit. openJII's bindings record which experiments a
device serves, and openJII will refuse publishes to an unbound experiment once
bindings are enforced at the broker (OJD-2581). So the GUI keeps them in step
(HAR-345), in two parts around the device change:

1. **Before any device write**, it first runs every read-only check (broker
   environment, topic shape, firmware, destination workbook), so a change that
   cannot happen adds no binding. It then finds the device on openJII by MAC
   and onboards it to the destination. A definite refusal (a 4xx answer) stops
   the action with "No device settings were changed" and the reason, even if
   an older binding to the destination exists. When the outcome is unknown (no
   answer, a 5xx or an unreadable reply), it reads the bindings back and goes
   on only if the destination is listed.
2. **After the change is verified**, and only when **After the change, also
   remove this Ambyte from its current experiment on openJII** is ticked (the
   default), it removes the binding to the experiment the device left. Removal
   deletes only the binding; measurements stay. A refusal is reported and the
   change stands; an unknown outcome is settled by reading the bindings back.

The real limit on who can run the action is the device: onboarding needs Can
edit or admin on it (organization owner or admin, or a Can edit grant). A plain
organization member or a Can view grantee is refused. openJII also refuses when
the account lacks contribute access to the destination or access to every
experiment the device is already onboarded to (a stale binding to someone
else's experiment blocks it, which is one reason removal is on by default),
when the destination is archived, or when the device certificate is not active.
Removal needs contribute access to the experiment being left. openJII answers
404 both for "not bound" and for an experiment it cannot find, reported as
"no binding found". When the device already sends to the destination (a retry
after a partial failure), the experiment it left is not known, so no binding
is removed and the GUI says so.

If onboarding succeeded but the device change then fails, the destination
binding stays. That is harmless while the device still publishes to its old
topic, and it is needed when the change is retried. Neither part registers,
issues or rotates anything.

```mermaid
sequenceDiagram
    participant GUI as Destination window
    participant API as openJII
    participant USB as Selected Ambyte
    GUI->>API: Read-only checks, resolve destination workbook
    GUI->>API: Find device by MAC, onboard to destination
    GUI->>API: Read bindings back
    alt destination bound
        GUI->>USB: Install workbook, write topics, reboot, verify
        USB-->>GUI: Verified destination
        opt remove from current experiment ticked
            GUI->>API: Remove binding to the experiment it left
        end
    else refused or unconfirmed
        GUI-->>GUI: Stop, no device settings were changed
    end
```

```mermaid
flowchart TD
    Start[Change experiment confirmed] --> Pre{Read-only checks pass?}
    Pre -->|no| Stop0[Stop: no binding, no device change]
    Pre -->|yes| Find{Device visible on openJII with this client name?}
    Find -->|no| Stop1[Stop: no device settings changed]
    Find -->|yes| Bind{Onboard to destination}
    Bind -->|4xx refusal| Stop2[Stop: openJII refused, reason shown]
    Bind -->|unknown outcome| Back{Destination listed on read-back?}
    Back -->|no| Stop3[Stop: binding not confirmed]
    Back -->|yes| Change
    Bind -->|200| Back
    Change[Install workbook, write topics, reboot, verify]
    Change -->|failed| Keep[Report stage; destination binding kept for the retry]
    Change -->|verified| Ticked{Remove from current experiment ticked?}
    Ticked -->|no| Done[Done, old binding kept]
    Ticked -->|yes| Remove[Remove old binding]
    Remove -->|204| Removed[Removed, data stays]
    Remove -->|404| None[No binding found]
    Remove -->|4xx| Reported[Refusal reported, change stands]
    Remove -->|unknown| Check[Read bindings back and report]
```

## Which environment and credentials?

The device's stored MQTT broker is its actual target. Selecting dev or prod in
the GUI selects the API account/workbook source and does not change that broker.
The display matches the broker against nonsecret endpoints learned from earlier
explicit onboarding responses. A missing or ambiguous match is **Unknown /
unverified**, never guessed from the selected API environment. A known mismatch
blocks the switch. Unknown requires explicit confirmation showing the broker,
API environment, destination experiment and device MAC side by side.

Certificate presence and validity are not exposed by this firmware's console.
Configuration being present is not proof that a certificate exists or is valid.
**Check cloud status** optionally reads the matching client from the selected
API environment's visible registry, showing its active/revoked and connectivity
status. It does not compare the USB device's certificate and is never a gate:
a registry permission failure does not imply lack of destination-experiment
access. The maintenance flow keeps the broker and credentials; cross-environment
provisioning remains a separate, explicit identity replacement.

## Direct handover

The destination workbook is fetched again using the device's running firmware
version, then stamped through the existing workbook compiler. This avoids
installing a cached workbook compiled for another release. Its journaled service
transfer and loaded SHA/workbook verification finish before topic writes.

Only the experiment UUID segment of the raw seven-segment ingest root changes.
Sensor type, version and sensor ID, including a literal `{MAC}`, are preserved.
The status topic is explicitly set to `<new-root>/status`; the command topic,
client ID, broker and name stay unchanged. The switch does not write timezone;
any device-managed drift is reported separately. The old `a3b865d1` migration
destination is refused because released firmware rewrites its status topic.
Unsupported topic shapes and lines exceeding the USB console buffer fail before
any maintenance write.

**Queued and transitional records follow the topic and workbook current when
published.** This switch neither drains nor discards the backlog and can run
offline after resolving the workbook. The device may briefly run the newly
installed workbook against the old topic until the reboot applies the topics.
No record deletion, cursor movement or delivery override is added.

```mermaid
sequenceDiagram
    actor Operator
    participant GUI as Destination window
    participant API as Selected API environment
    participant USB as Selected Ambyte
    GUI->>USB: Read loaded identity and public MQTT config
    USB-->>GUI: MAC, broker, topic and workbook
    GUI-->>Operator: Show broker environment or Unknown
    Operator->>GUI: Confirm destination and preserved broker
    GUI->>API: GET experiment, pinned workbook and macros
    API-->>GUI: Accessible workbook programming
    GUI->>USB: Recheck MAC, configuration and loaded schedule
    GUI->>USB: Journaled schedule transfer and commit
    USB-->>GUI: Durable success and matching loaded SHA/workbook
    GUI->>USB: Set ingest root and status topic, read both back
    GUI->>USB: Reboot
    GUI->>USB: Reconnect only to the same USB MAC
    USB-->>GUI: Expected topics, workbook and preserved broker/client
    GUI-->>Operator: Verified experiment change
```

## Failures and recovery

The serial-action gate remains held while the window is open. Writes compare
the preview's identity and config on the same descriptor. Startup is allowed to
finish before comparing the loaded schedule. The MAC-keyed journal lease spans
the change: a different incomplete operation must be resolved first; a matching
workbook transfer can resume through the existing service client. The action
never rotates certificates as recovery.

A failed or uncertain workbook install performs no topic write. After the
workbook succeeds, a cfg failure truthfully reports that topics may be partial:
either setter might have persisted before the failure. Read the destination and
explicitly repeat the action. Topic construction is idempotent, including when
the ingest root already contains the new experiment. After reboot, enumerate
ports without opening unrelated devices and open only the matching USB serial.
Transient disappearance/boot errors retry within the reconnect deadline. Drivers
that omit the serial require an operator refresh rather than a fleet scan.

```mermaid
flowchart TD
    Read[Read device destination] --> Verify{Destination and broker confirmed}
    Verify -->|No| Read
    Verify -->|Yes| Workbook[Resolve and install pinned workbook]
    Workbook -->|Failed or uncertain| Recover[Recover saved operation, no topic writes]
    Workbook -->|Loaded identity matches| Topics[Write and read back both topics]
    Topics -->|Failure| Partial[Topics may be partial, read and repeat]
    Topics -->|Match| Boot[Reboot same MAC and verify]
    Boot -->|Mismatch or unavailable| Inspect[Verification incomplete, read and repeat]
    Boot -->|Match| Done[Experiment and workbook verified]
```

The final check requires unchanged MAC, client identity, firmware, broker,
command topic and device name, the expected ingest/status topics, and
an INSTALLED schedule with the exact target SHA and workbook version.

## Qualification

Host tests exercise real console bytes and the service client for the direct
switch, raw topic preservation, environment consent/mismatch, destination 403,
board swaps, compile rejection, partial cfg writes and rerun, startup settling,
wrong loaded workbook, transient re-enumeration and avoiding unrelated devices.
GUI tests cover read-only use without sign-in, confirmation, worker boundaries,
partial-result reporting and unverified credential labels.

Physical qualification on Ambyte `28:37:2F:FF:E7:04`, firmware 2.13.0,
passed using the production maintenance backend and an explicit synthetic API /
workbook fixture. The device was kept offline throughout the synthetic
experiment phase. The fixture workbook had only a manually dispatched log job,
with no sensor actions. The change installed that workbook, changed both topics,
rebooted and passed the production post-reboot checks while preserving broker
and client identity. No synthetic experiment was published to the cloud.

Before reconnecting Wi-Fi, the original topics and exact cached workbook bytes
(SHA `943c294243a0254b364172dd3252069e350cbc54428fed88fb84f65c642ea2b7`)
were restored and verified. The original Wi-Fi then connected after reboot, and
firmware, client, topics, workbook and certificate fingerprints matched the
baseline. Private restoration backups were removed. This proves the USB path
on that image; it does not claim end-to-end ingestion into a live destination.

A real Tk lifecycle check with a synthetic device-read fixture verified modal
construction, worker completion, the Unknown environment label, disabled writes
without sign-in, grab release and busy release. The complete host GUI suite
passes **528 tests and 29 subtests**; the production C wire/filter suite passes
**11 tests and 13 subtests** under ASan/UBSan with `CC=clang`.

![Experiment change and original-state restoration](images/usb-experiment-maintenance/qualification.png)

[Redacted qualification transcript](images/usb-experiment-maintenance/qualification.txt).

## Automatic checks on maintenance reads

The destination window also performs the [automatic workbook and deployment
checks](usb-maintenance-sync.md) on read. It uses the device's current experiment,
regardless of the destination selected in the main window. The direct Change
experiment action still installs and verifies its selected workbook before topic
writes; afterward the deployment timezone check can ask for a separate correction.
The workbook/timezone results are shown independently of the experiment result.

Device-managed timezone drift after experiment switching is graded through the
same function as firmware maintenance: an explained location-derived change is
informational; an unexplained configured/manual change has a visible needs-attention
warning with both values. It cannot silently disappear from the primary result.

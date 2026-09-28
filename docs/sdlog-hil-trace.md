# sd_logger HIL trace, inventory and export grammar (Sprint 2, verification build only)

Everything here is compiled only with `CONFIG_AMBYTE_EVQ_HIL` (env `evq-hil`) and
is absent from the release image (`tools/check_release_no_hil.py`). It is the
wire format between the firmware (`components/event_log/evq_hil_sdl.c`, hooks in
`components/sd_logger/sd_logger.c`, commands in `components/evq_hil/evq_hil.c`)
and the host checkers (`tools/evq_hil/sdlog_check.py`). Frozen by the Sprint 2
contract (sha256 `a722b3f2…`, H1-H8).

All lines are printed with `printf` (never `ESP_LOG`), `\r\n`-terminated on the
console; parsers must accept `\n` or `\r\n` and ignore unrelated interleaved
console lines. Integers are decimal; `<us>` is `esp_timer_get_time()`; `<sha>`
is lowercase hex sha256; `<b64>` is standard base64 with padding, no line breaks.

## Logger trace (H2) — `evq_hil sdlog_trace <on|off|drain|autoarm|stat|probe <min_free>>`

- `on` allocates a 128 KiB PSRAM byte ring (once) and starts recording:
  `SLT_ON <cap_bytes> <next_seq>`. The allocation fails closed: if less than
  192 KiB of PSRAM would stay free, the ring is released again and `on` prints
  `SLT_ERR noalloc free=<bytes>`. Sized from the E8:F6:0A bench's measured
  runtime headroom (393,228 B free of 2 MiB); drain every 2 s.
- `probe <min_free>` runs the same allocation with a caller-chosen floor, but
  does not start recording (G-TR fail-closed check):
  `SLT_PROBE need=<min_free> before=<free> after=<free> ring=<new|kept|none> <ok|noalloc>`.
  `probe 1073741824` must answer `ring=none noalloc` with `after == before`.
- `off` stops recording: `SLT_OFF <next_seq>`. Undrained entries stay drainable.
- `drain` prints every undrained entry, then the header:
  ```
  SLT_DRAIN <first_seq> <last_seq>          (first>last when empty)
  SLT_E ... one line per entry, seq ascending ...
  SLT_HDR <total_recorded> <lost> <first_seq> <last_seq> <fill_bytes_before_drain>
  ```
  `seq` is global and contiguous across drains for the whole boot; `lost` is
  cumulative (entries not stored because the ring was full). `total_recorded`
  counts stored entries since boot.
- `autoarm` sets an RTC_NOINIT one-shot flag (magic+CRC): `SLT_AUTOARM set`.
  On the next boot the hook at the start of `sd_logger_init` clears it and
  starts recording before the writer task exists: `SLT_ON <cap> <next_seq> autoarm`.
  Invalid after power-on / bad CRC.
- `stat`: `SLT_STAT <recording 0|1> <next_seq> <lost> <fill_bytes> <cap>`.
- Watermark: when fill first crosses 25 % of capacity since the last drain the
  device prints `SLT_WM <fill_bytes>` once (re-armed by each drain).

Entry lines (`SLT_E <kind> <seq> <us> ...`):

| kind | fields | recorded where |
| --- | --- | --- |
| `P` | `<len> <sha> <pushed\|dropped> <syn 0\|1> <b64>` | `ring_push`, inside the ring lock, with the exact framed record bytes (sha computed at drain from the stored bytes). `syn`=1 iff the record contains ` HILSDLOG: ` (the emitter tag as the IDF v1 format renders it; contract H2's "`HILSDLOG `" read as that tag — a convenience flag, the host recomputes it from the bytes). |
| `POP` | `<n>` | `ring_pop`, inside the ring lock, when n > 0: the next n pushed bytes left the ring (whole records, FIFO). |
| `WR` | `<n> <written> <before>` | `write_chunk` after `fwrite`: `written` of the chunk's `n` bytes went to the file at offset `before`. |
| `COMMIT` | `<committed>` | `commit()` success. |
| `RB` | `<to> <since> <ok\|quar_trunc\|quar_fsync>` | `rollback()`: `ok` = truncated to `to` and synced; `quar_trunc` = ftruncate failed (file left as written); `quar_fsync` = ftruncate done, fsync failed. |
| `ROT` | `<ok\|fail> <op> <errno> [step=<src>]` | `rotate_files()`: `op` = `remove`/`rename`/`-`; on `fail rename`, `step=<src>` names the failing rename's source index (rename `ambyte.<src>` → `<src+1>`; renames of higher sources were applied). `ok` means remove(ambyte.5) then renames 4→5 … 0→1. |
| `OPEN` | `<name> <size> <torn 0\|1>` | `open_log()` success. |
| `CLOSE` | `<ok\|err\|abandon>` | `close_log()` / `abandon_log()`. |
| `DROP` | `<rotate_blocked\|unavailable> <n>` | `drop_chunk()`: a popped chunk not written. |
| `QUI` | `<paused\|resumed\|timeout\|refused>` | H7 quiesce. |

## Reset witness (H8a)

On a `reset_mid_write` firing whose writer is `sdlog`, after the half write +
fflush + fsync and before the reset:
```
SLT_RESET_WITNESS <path> <file_off_before> <n> <half> <sha(chunk n bytes)> <b64(chunk n bytes)>
SLT_DRAIN … SLT_E … SLT_HDR …        (synchronous final drain)
```
followed by the existing `HIL_FAULT fired kind=cpu_reset …` line; stdout is
flushed and the USB TX FIFO given ≤ 500 ms before `esp_rom_software_reset_system`.

## Emitter (H3) — `evq_hil sdlog_emit <run> <k0> <n> <hz> <pad>`

Task priority 1; for k in [k0, k0+n): `ESP_LOGW("HILSDLOG", "%s %u %s", run, k, p)`
where `p = evq_hil_pad(run, k, pad)` (same xorshift as `evq_hil_payload`, pad
characters only; host twin `hilpay.pad`). Constraints: run matches
`[A-Za-z0-9._-]{1,32}`, pad ≤ 160, hz 1..50, n ≥ 1. Prints
`SDL_BEGIN <run> <k0> <n> <hz> <pad> <us>` and on completion
`SDL_END <run> <k_last> <us>`. Refuses a second emitter: `SDL_ERR busy`.

The framed record is (log v1, colors off, RTOS ms timestamp):
`<YYYY-MM-DD HH:MM:SS>␠␠W␠(<uptime_ms>)␠HILSDLOG:␠<run>␠<k>␠<p>\n`.

## Quiesce (H7) and inventory/export (H4/H5)

Quiesce preconditions: external power (Vin present) and the low-battery guard not
parked; otherwise `SDL_Q refused guard`. Then `sd_logger_pause()` and wait
≤ 10 s for `sd_logger_hil_paused()`; timeout → resume, `SDL_Q timeout`.
Success → `SDL_Q paused`; every exit path → `sd_logger_resume()` and
`SDL_Q resumed`. A guard transition during the quiesce → `SDL_INVALID guard`.

`evq_hil sdlog_inv` (no args), for each regular file in `/sdcard/logs` sorted by name:
```
SDL_FF <name> <size> <sha> <lines> <torn 0|1> <maxline>
```
then `SDL_STATE <cur_name> <committed> <quar 0|1> <backoff_ms_left>` and
`SDL_INV_END <nfiles>`. `lines` = count of `\n`; `torn` = size>0 and last byte
≠ `\n`, or any line (incl. its `\n`) longer than 256 B; `maxline` = longest line
length incl. `\n`.

`evq_hil sdlog_inv <name> <from_off>`:
```
SDL_FL <name> <off> <len> <sha(line bytes incl. \n)>     (≤ 20000 per call)
SDL_MORE <next_off>                                     (only if truncated)
SDL_INV_END 1
```
A trailing unterminated tail is reported as an `SDL_FL` with its length and no
`\n` (the host sees torn from `SDL_FF`).

`evq_hil sdlog_dump <name> [<from_off>]`:
```
SDL_B64 <off> <b64 of ≤ 3072 bytes>        (repeated, offsets contiguous)
SDL_DUMP_END <from_off> <size> <sha(whole file)> <sha(from_off..size)>
```

Errors: `SDL_INVALID <name|-> <reason>`, `SDL_TIMEOUT <name>`. Each file is
opened, fully read and closed inside one `sdcard_io_begin/end` hold (≤ 3 s/MiB
inventory, ≤ 10 s/MiB dump); whole command ≤ 180 s (inventory) or
300 s/MiB (dump).

## Injector slots (H1)

`evq_hil fault io <w> <op> <mode> [nth] [count] [path=<substr>]` clears both slots
and arms A; `evq_hil fault io add <...same...>` arms B (refused without A or
when B busy: `HIL_ERR fault io add: ...`). Each wrapped op is offered to A then
B; a slot counts an op only when writer, op and (if given) path substring
match; at most one slot fires per op. Firing line gains ` slot=<A|B>`.
`fault io any any off` clears both.

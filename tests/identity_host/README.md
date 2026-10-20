# Identity host harness

Run `CC=clang python3 -m unittest discover -s tests -p test_identity.py -v`.
Requires a C11 compiler, ASan/UBSan, pthreads and libcrypto development headers.

- `handler.c` links the same `components/CLI/cli_identity.c` that the firmware
  registers directly. SDK/provider boundaries inject device/loaded/release
  identities. Network, SD, unlock, config and queue dependencies are absent;
  adding one makes the link fail. A wrapped production fwrite coordinates a
  competing stdout writer and proves it blocks during real response output.
- `runner.c` includes the whole production `sched_runner.c`, seeding its private
  lifecycle/program/source. It exercises unavailable compile windows, atomic
  source/workbook/generation snapshots and stopped-but-loaded identity.
- `provider.c` links the whole production `script_update.c`. Only file paths,
  NVS and mbedTLS's crypto boundary are replaced. The file wrapper permits only
  `rb`, the NVS open permits only READONLY, and writes abort. SHA-256 is computed
  by OpenSSL over the real bytes read by the production provider. Allocation
  and fread wrappers also check that hashing requests no heap scratch buffer
  and leaves stdio unbuffered (rather than allocating a 4 KiB read buffer).

Section GC drops unused hardware paths; no production function is copied or
extracted. Host SDK declarations are in `include/` and `tests/host_stubs`.
These tests prove host behavior, not ESP32/newlib scheduling: the firmware's
normal esp_log v1 console tee uses vprintf on the stdout FILE, and bench checks
race actual logs against physical response lines. ROM/early/panic writers
bypass stdout locking and remain outside that guarantee.

`bench.yaml` is an explicit test-workbook, log-only fixture scheduled on
January 1. Use only after obtaining the exact original installed bytes and
release tuple for restoration. Confirm runs=0 before restoring; it never
acquires sensors or stores events even if its log-only trigger fires.

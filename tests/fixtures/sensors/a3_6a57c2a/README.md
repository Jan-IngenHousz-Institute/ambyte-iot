# A3 reference sources for the sensor trace golden

These are unmodified source blobs from A3 candidate commit
`6a57c2ae155597a81dc1b0a010824c7e9336f0a7`. A3 was squash-merged, so this
candidate is not guaranteed to exist in a fresh CI checkout.

| Fixture | Original source path | Bytes | SHA256 |
| --- | --- | ---: | --- |
| `device_commands.c` | `components/device_commands/device_commands.c` | 151,734 | `0b6d99b90a7d24f672d3e37190358d3c0d66d4e57036486e7408c33dd003f8b5` |
| `ambit_trace.c` | `components/ambit_trace/ambit_trace.c` | 20,820 | `8f9c6628c9468d1edc92e37d75a888d0601a4a77a474536e86d2ce8f96ccac5b` |

`tests/test_sensors_controller.py` verifies each digest before compiling
the reference. The test still requires ten byte-identical trace records
and zero added events, NVS setters and commits on the production candidate.
These are reference inputs, not rewritten expected outputs. Keep their
original bytes, including whitespace; do not format or update them.

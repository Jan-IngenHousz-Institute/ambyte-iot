# Released A2 parser

`ambyte_serial_6b86c8e.py` is the unmodified `flash_gui/ambyte_serial.py`
from commit `6b86c8e33fc19149f4c5661545143653aa919705`, the released
flash-gui v0.2.22 parser used by A4's additive-capability compatibility gate.

| Fixture | Bytes | SHA256 |
| --- | ---: | --- |
| `ambyte_serial_6b86c8e.py` | 25,560 | `eb8c527c4eac21d28cd84e5998e89fff39e5334e4e95fce3903b3d462e2bc0c3` |

The compatibility test verifies this digest before importing the parser.
It still parses an identity carrying both `svc` and `sensors` with the real
released implementation. It needs neither git history nor a release tag.
Keep the snapshot byte-for-byte; do not format or update it with current code.

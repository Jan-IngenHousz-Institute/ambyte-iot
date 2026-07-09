#!/usr/bin/env bash
# macOS/Linux launcher for tools/flash_certs.py — finds a Python and forwards args.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/flash_certs.py"
VENVPY="$HERE/../.venv/bin/python"

if [ -x "$VENVPY" ]; then
  exec "$VENVPY" "$SCRIPT" "$@"
fi
for py in python3 python; do
  if command -v "$py" >/dev/null 2>&1; then
    exec "$py" "$SCRIPT" "$@"
  fi
done
echo "Python 3 not found. Install it (e.g. brew install python) and re-run." >&2
exit 1

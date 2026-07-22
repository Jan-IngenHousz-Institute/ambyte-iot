#!/usr/bin/env bash
# macOS/Linux launcher for tools/flash_certs.py — finds a Python and forwards args.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/flash_certs.py"
VENVPY="$HERE/../.venv/bin/python"

# A .venv copied from another machine has a python that only redirects to a base
# interpreter that isn't here — probe it before trusting it.
if [ -x "$VENVPY" ] && "$VENVPY" -c "" >/dev/null 2>&1; then
  exec "$VENVPY" "$SCRIPT" "$@"
fi
if [ -e "$VENVPY" ]; then
  echo "[warn] ignoring unusable .venv (base interpreter missing?); falling back to system Python." >&2
fi
for py in python3 python; do
  if command -v "$py" >/dev/null 2>&1; then
    exec "$py" "$SCRIPT" "$@"
  fi
done
echo "Python 3 not found. Install it (e.g. brew install python) and re-run." >&2
exit 1

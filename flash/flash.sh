#!/usr/bin/env bash
# =====================================================================
#  ambyte firmware flasher  (NO COMPILE NEEDED) - launcher for flash.py
#  Reads the board MAC, checks allowed_macs.txt, then writes the images
#  in bin/ with esptool. Nothing is compiled.
#
#  Usage:  ./flash.sh [options]         (full help:  ./flash.sh --help)
#     ./flash.sh                 auto-detect port, gate, confirm, flash
#     ./flash.sh --port /dev/ttyACM0
#     ./flash.sh --any           bypass the allow-list
#     ./flash.sh --list          print the allow-list and exit
# =====================================================================
set -eo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/flash.py"

# Prefer PlatformIO's penv python (carries pyserial); else python3/python on
# PATH. flash.py resolves esptool itself.
CORE="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
for c in "$CORE/penv/bin/python" "$CORE/penv/bin/python3" "$CORE/penv/Scripts/python.exe"; do
  [ -x "$c" ] && exec "$c" "$SCRIPT" "$@"
done
for p in python3 python; do
  command -v "$p" >/dev/null 2>&1 && exec "$p" "$SCRIPT" "$@"
done
echo "[error] Python 3 not found. Install PlatformIO or Python 3, then re-run." >&2
exit 1

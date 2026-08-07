"""esptool operations, via the esptool>=5 scripting API (no shelling out).

Three entry points:
  read_mac(port)                        works even on an unflashed chip
  flash_images(port, images, ...)       full flash: release images + nvs.bin
  write_nvs(port, nvs_path)             provision-only rewrite at 0x9000

Each opens its own esptool session (detect → stub → attach → op → hard reset),
because the ESP32-S3 native USB-Serial-JTAG re-enumerates on reset — holding a
connection across steps is what breaks, not reconnecting.

A failed/interrupted write_flash leaves the chip in the ROM/stub bootloader,
which is exactly the re-flashable state the retry path needs — no cleanup is
required beyond closing the port.
"""

from __future__ import annotations

import io
import re
import sys
from contextlib import redirect_stdout
from pathlib import Path

from esptool.cmds import attach_flash, detect_chip, reset_chip, write_flash

# esptool's own default; generous because USB-JTAG ports can take a moment to
# come back after a reset.
CONNECT_ATTEMPTS = 7
FLASH_BAUD = 460800

# esptool paints progress with ANSI cursor/erase sequences and \r overdraws;
# both are noise in a GUI log line and garbage in an error dialog.
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def _clean(text: str) -> str:
    return _ANSI_RE.sub("", text).replace("\r", "\n")


def _close_port(esp) -> None:
    """Close the loader's pyserial object. NB: esp.serial_port is a property
    returning the port NAME (a str) — the Serial object is esp._port."""
    port = getattr(esp, "_port", None)
    close = getattr(port, "close", None)
    if callable(close):
        try:
            close()
        except Exception:
            pass


class EsptoolError(RuntimeError):
    """Any esptool-level failure, with the captured tool output attached."""


class _Tee(io.StringIO):
    """Capture esptool's prints while forwarding them line-wise to a logger.

    Progress overdraws (\r-updated bars) collapse to their final state and
    ANSI is stripped, so the GUI log stays readable.
    """

    def __init__(self, log):
        super().__init__()
        self._log = log
        self._buf = ""

    def write(self, s: str) -> int:
        n = super().write(s)
        if self._log:
            self._buf += _clean(s)
            while "\n" in self._buf:
                line, self._buf = self._buf.split("\n", 1)
                line = line.strip()
                # Drop intermediate progress frames; keep only landmark lines.
                if line and not line.startswith("Writing at"):
                    self._log(line)
        return n

    def tail(self, lines: int = 12) -> str:
        """The last few meaningful output lines, for error messages."""
        cleaned = [ln.strip() for ln in _clean(self.getvalue()).splitlines()
                   if ln.strip() and not ln.strip().startswith("Writing at")]
        return "\n".join(cleaned[-lines:])


def _session(port: str, baud: int):
    """Connect and return a stub-running ESPLoader. Caller must hard-reset."""
    esp = detect_chip(port=port, baud=115200,
                      connect_attempts=CONNECT_ATTEMPTS)
    if esp.CHIP_NAME.lower().replace("-", "") != "esp32s3":
        try:
            reset_chip(esp, "hard-reset")
        finally:
            _close_port(esp)
        raise EsptoolError(
            f"connected chip is {esp.CHIP_NAME}, expected ESP32-S3 — wrong "
            "board or wrong port.")
    esp = esp.run_stub()
    if baud != 115200:
        try:
            esp.change_baud(baud)
        except Exception:
            pass   # stay at 115200 — slower, not wrong
    return esp


def read_mac(port: str, log=None) -> str:
    """The base (Wi-Fi STA) MAC as AA:BB:CC:DD:EE:FF — the board's serial
    number in the openJII registry and in the AMBYTE_<MAC> naming scheme."""
    tee = _Tee(log)
    try:
        with redirect_stdout(tee):
            esp = detect_chip(port=port, baud=115200,
                              connect_attempts=CONNECT_ATTEMPTS)
            try:
                mac = esp.read_mac("BASE_MAC")
            finally:
                try:
                    reset_chip(esp, "hard-reset")
                finally:
                    _close_port(esp)
    except EsptoolError:
        raise
    except Exception as exc:
        raise EsptoolError(f"reading MAC on {port} failed: {exc}\n"
                           f"{tee.tail()}") from exc
    return ":".join(f"{b:02X}" for b in mac)


def flash_images(port: str, images: list[tuple[int, Path]],
                 flash_settings: dict[str, str], log=None) -> None:
    """Write all images (offset asc). Writes only these regions — no chip
    erase — so field data in coredump/littlefs/storage survives a re-flash."""
    _write(port, images, flash_settings, log)


def write_nvs(port: str, nvs_offset: int, nvs_path: Path, log=None) -> None:
    """Provision-only rewrite: just the NVS partition, app untouched."""
    # NVS is data, not an app image: 'keep' leaves the flash header alone.
    _write(port, [(nvs_offset, nvs_path)],
           {"flash_mode": "keep", "flash_size": "keep", "flash_freq": "keep"},
           log)


def _write(port: str, images: list[tuple[int, Path]],
           flash_settings: dict[str, str], log=None) -> None:
    for _, path in images:
        if not Path(path).is_file():
            raise EsptoolError(f"image missing: {path}")
    tee = _Tee(log)
    try:
        with redirect_stdout(tee):
            esp = _session(port, FLASH_BAUD)
            try:
                attach_flash(esp)
                addr_data = [(off, open(path, "rb")) for off, path in images]
                try:
                    write_flash(
                        esp, addr_data,
                        flash_mode=flash_settings.get("flash_mode", "keep"),
                        flash_size=flash_settings.get("flash_size", "keep"),
                        flash_freq=flash_settings.get("flash_freq", "keep"),
                    )
                finally:
                    for _, fh in addr_data:
                        try:
                            fh.close()
                        except Exception:
                            pass
            finally:
                try:
                    reset_chip(esp, "hard-reset")
                finally:
                    _close_port(esp)
    except EsptoolError:
        raise
    except Exception as exc:
        raise EsptoolError(f"esptool write on {port} failed: {exc}\n"
                           f"{tee.tail()}") from exc


def list_serial_ports() -> list[dict]:
    """All serial ports, ESP32 USB-Serial-JTAG ones first."""
    from serial.tools import list_ports

    from .config import USB_JTAG_PID, USB_JTAG_VID
    out = []
    for p in list_ports.comports():
        is_jtag = (p.vid == USB_JTAG_VID and p.pid == USB_JTAG_PID)
        out.append({"device": p.device,
                    "description": p.description or "",
                    "is_ambyte_jtag": is_jtag})
    out.sort(key=lambda e: (not e["is_ambyte_jtag"], e["device"]))
    return out


if __name__ == "__main__":   # tiny manual smoke test:  python -m flash_gui.esptool_ops COM7
    print(read_mac(sys.argv[1], log=print))

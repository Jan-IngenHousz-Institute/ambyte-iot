"""PlatformIO post-upload hook — set the device RTC to the host's current UTC
time over the serial console, right after flashing.

Registered in platformio.ini as `post:tools/set_rtc_on_upload.py`. The action is
attached to the `upload` target, so it runs only on `pio run -t upload` (and the
IDE Upload button), never on a plain build.

It reuses the firmware's `rtc set <epoch>` CLI command, so no firmware change is
needed. A factory-fresh RTC reports its time as invalid (oscillator-stop flag)
until set; this brings it online with accurate wall-clock time at provisioning.

Behaviour:
  * Opens the upload serial port at monitor_speed after the flash + reset.
  * Retries `rtc set <now-UTC-epoch>` until the CLI answers "RTC set" or the
    window elapses (the CLI comes up ~20-35 s into boot — Wi-Fi join + UART
    sensor pings run first).
  * A failure here is logged loudly but does NOT fail the upload (the firmware is
    already flashed; set the clock later with `rtc set <epoch>` or let the hourly
    RTC sync pick it up once set).

Skip with AMBYTE_RTC_SKIP=1.
"""

import os
import sys
import time

Import("env")  # noqa: F821 — provided by PlatformIO/SCons

# The CLI is only responsive late in boot; allow generous time and retry.
_BOOT_SETTLE_S = 4.0      # let the initial boot delay pass before first send
_TOTAL_WINDOW_S = 45.0    # give up after this (device absent / port busy)
_ATTEMPT_GAP_S = 3.0      # between send attempts
_CONFIRM_WAIT_S = 2.5     # read for the device's reply after each send


def _truthy(value):
    return (value or "").strip().lower() in ("1", "true", "yes", "on")


def _resolve_port(env):
    port = env.subst("$UPLOAD_PORT") or ""
    if not port:
        try:
            port = env.GetProjectOption("monitor_port") or ""
        except Exception:
            port = ""
    if not port:
        try:
            port = env.AutodetectUploadPort() or env.subst("$UPLOAD_PORT") or ""
        except Exception:
            port = ""
    return port


def _resolve_baud(env):
    try:
        return int(env.GetProjectOption("monitor_speed") or 115200)
    except Exception:
        return 115200


def _set_rtc(source, target, env):  # noqa: ARG001 — SCons post-action signature
    if _truthy(os.environ.get("AMBYTE_RTC_SKIP")):
        print("ambyte-rtc: AMBYTE_RTC_SKIP set — not setting RTC")
        return

    try:
        import serial  # PlatformIO core ships pyserial
    except ImportError:
        print("ambyte-rtc: pyserial unavailable — RTC NOT set", file=sys.stderr)
        return

    port = _resolve_port(env)
    if not port:
        print("ambyte-rtc: could not determine the serial port — RTC NOT set; "
              "set it manually with `rtc set <epoch>`", file=sys.stderr)
        return
    baud = _resolve_baud(env)

    print(f"ambyte-rtc: setting RTC over {port} @ {baud} (UTC) ...")
    try:
        ser = serial.Serial(port, baud, timeout=0.5)
    except Exception as exc:
        print(f"ambyte-rtc: cannot open {port}: {exc} — RTC NOT set "
              "(close any open serial monitor, then `rtc set <epoch>`)", file=sys.stderr)
        return

    ok = False
    set_epoch = None
    try:
        time.sleep(_BOOT_SETTLE_S)
        deadline = time.time() + _TOTAL_WINDOW_S
        while time.time() < deadline and not ok:
            # Fresh epoch every attempt so the value stays accurate as we wait
            # for the CLI to come up. time.time() is UTC-based Unix seconds.
            set_epoch = int(time.time())
            try:
                ser.reset_input_buffer()
                ser.write(f"\r\nrtc set {set_epoch}\r\n".encode())
                ser.flush()
            except Exception as exc:
                print(f"ambyte-rtc: serial write failed: {exc}", file=sys.stderr)
                break

            reply = b""
            reply_deadline = time.time() + _CONFIRM_WAIT_S
            while time.time() < reply_deadline:
                chunk = ser.read(256)
                if chunk:
                    reply += chunk
                    if b"RTC set" in reply:
                        ok = True
                        break
                    if b"rtc set failed" in reply or b"RTC read error" in reply:
                        break
            if ok:
                # Echo the device's confirmation line(s).
                for line in reply.decode(errors="replace").splitlines():
                    if "RTC" in line:
                        print(f"ambyte-rtc: {line.strip()}")
                break
            time.sleep(_ATTEMPT_GAP_S)
    finally:
        ser.close()

    if ok:
        human = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(set_epoch))
        print(f"ambyte-rtc: RTC set to {human} (epoch {set_epoch})")
    else:
        print("ambyte-rtc: WARNING — RTC was NOT confirmed set. The firmware is "
              "flashed; set the clock with `rtc set <epoch>` over the console, or "
              "it will auto-sync once the RTC holds a valid time.", file=sys.stderr)


env.AddPostAction("upload", _set_rtc)  # noqa: F821

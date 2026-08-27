# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only

"""Tkinter GUI: session settings on top, one-button per-device procedure below.

Threading model: all serial/esptool/HTTP work runs on a single worker thread
per action; the Tk main loop is never blocked. Workers talk back exclusively
via `_post` (thread-safe `after` scheduling); the one point where a worker
needs an answer from the operator (the name prompt) blocks the worker on a
Queue that the main-thread dialog fills.
"""

from __future__ import annotations

import queue
import sys
import threading
import tkinter as tk
import webbrowser
from datetime import datetime, timezone as dt_timezone
from tkinter import messagebox, scrolledtext, simpledialog, ttk
from tkinter import font as tkfont

from . import app_update, esptool_ops, procedure, release_fetch, timezones
from .config import ENVIRONMENTS, Settings
from .openjii_client import OpenJIIClient, OpenJIIError
from .procedure import (DeviceRun, ProcedureError, SessionContext,
                        clean_device_name)

STEPS = [
    ("check", "Pre-flash check"),
    ("credentials", "openJII onboarding"),
    ("nvs", "NVS image"),
    ("flash", "Flash"),
    ("provision", "RTC + reboot"),
    ("lua", "Lua script"),
    ("verify", "Verify"),
]
STEP_ICONS = {"pending": "·", "running": "▶", "ok": "✓", "fail": "✗"}
UI_SCALE_MULTIPLIER = 2.0
UI_FONT_CANDIDATES = (
    "liberation sans", "dejavu sans", "noto sans", "cantarell",
    "segoe ui", "sf pro text", "arial", "helvetica",
)
FIXED_FONT_CANDIDATES = (
    "liberation mono", "dejavu sans mono", "noto sans mono",
    "courier new", "courier 10 pitch",
)


def _available_font(root: tk.Tk, candidates: tuple[str, ...]) -> str:
    available = {family.casefold(): family
                 for family in tkfont.families(root=root)}
    for candidate in candidates:
        if candidate in available:
            return available[candidate]
    return candidates[-1]


def apply_ui_scale(root: tk.Tk,
                   multiplier: float = UI_SCALE_MULTIPLIER) -> None:
    """Double widget geometry and rendered text, including bitmap defaults."""
    current = float(root.tk.call("tk", "scaling"))
    root.tk.call("tk", "scaling", current * multiplier)
    ui_family = _available_font(root, UI_FONT_CANDIDATES)
    fixed_family = _available_font(root, FIXED_FONT_CANDIDATES)
    for name in tkfont.names(root=root):
        named_font = tkfont.nametofont(name, root=root)
        target_pixels = max(1, round(
            int(named_font.metrics("linespace")) * multiplier))
        family = fixed_family if name == "TkFixedFont" else ui_family
        # Negative Tk font sizes are pixels, so this remains readable even
        # when a desktop maps Helvetica/Courier to the unscalable X11
        # ``fixed`` bitmap font.
        named_font.configure(family=family, size=-target_pixels)


class App(ttk.Frame):
    def __init__(self, master: tk.Tk):
        super().__init__(master, padding=10)
        self.installed_gui = app_update.load_installed_release()
        self.is_packaged = bool(getattr(sys, "frozen", False))
        title_version = (
            f"v{self.installed_gui.version}"
            if self.installed_gui is not None
            else ("version unknown" if self.is_packaged else "development")
        )
        master.title(f"ambyte on-boarding — GUI {title_version}")
        master.minsize(760, 700)
        self.pack(fill="both", expand=True)

        self.settings = Settings.load()
        self.client: OpenJIIClient | None = None
        self.user_label = tk.StringVar(value="not signed in")
        self.release: release_fetch.ReleaseImages | None = None
        self.lua_catalog: release_fetch.LuaCatalogRelease | None = None
        self.experiments: list = []
        self.current_run: DeviceRun | None = None
        self.busy = False
        self.local_tz = timezones.local_iana_zone()
        self.gui_release_url = app_update.RELEASES_PAGE

        self._build_application_frame()
        self._build_session_frame()
        self._build_info_frame()
        self._build_device_frame()
        self._tick_clock()

        self._refresh_ports()
        self._fetch_gui_update_async()
        self._fetch_release_async()
        self._fetch_lua_catalog_async()
        if self.settings.api_key(self.settings.environment):
            self._validate_key_async(self.settings.api_key(
                self.settings.environment), on_startup=True)

    # ── layout ───────────────────────────────────────────────────────────
    def _build_application_frame(self) -> None:
        frame = ttk.LabelFrame(self, text="Application version", padding=8)
        frame.pack(fill="x", pady=(0, 8))
        frame.columnconfigure(0, weight=1)

        if self.installed_gui is not None:
            version_text = f"Running GUI version: {self.installed_gui.version}"
        elif self.is_packaged:
            version_text = "Running GUI version: UNKNOWN (legacy package)"
        else:
            version_text = "Running from source (development build)"
        self.app_version_var = tk.StringVar(value=version_text)
        ttk.Label(
            frame,
            textvariable=self.app_version_var,
            font="TkHeadingFont",
        ).grid(row=0, column=0, sticky="w")

        self.app_update_var = tk.StringVar(value="Checking for updates…")
        self.app_update_label = ttk.Label(
            frame, textvariable=self.app_update_var, foreground="#555555"
        )
        self.app_update_label.grid(row=1, column=0, sticky="w", pady=(3, 0))

        self.app_release_btn = ttk.Button(
            frame,
            text="Open releases",
            command=self._open_gui_release,
            state="disabled",
        )
        self.app_release_btn.grid(
            row=0, column=1, rowspan=2, padx=(12, 0), sticky="e"
        )
        self.app_check_btn = ttk.Button(
            frame, text="Check again", command=self._fetch_gui_update_async
        )
        self.app_check_btn.grid(
            row=0, column=2, rowspan=2, padx=(8, 0), sticky="e"
        )

    def _build_session_frame(self) -> None:
        frame = ttk.LabelFrame(self, text="Session (set once)", padding=8)
        frame.pack(fill="x")
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="Environment:").grid(row=0, column=0, sticky="w")
        self.env_var = tk.StringVar(value=self.settings.environment)
        env_box = ttk.Combobox(frame, textvariable=self.env_var,
                               values=list(ENVIRONMENTS), state="readonly",
                               width=8)
        env_box.grid(row=0, column=1, sticky="w")
        env_box.bind("<<ComboboxSelected>>", lambda e: self._on_env_changed())

        self.login_btn = ttk.Button(frame, text="Sign in (API key)...",
                                    command=self._sign_in)
        self.login_btn.grid(row=0, column=2, sticky="e", padx=(8, 0))
        ttk.Label(frame, textvariable=self.user_label,
                  foreground="gray").grid(row=0, column=3, sticky="e",
                                          padx=(8, 0))

        ttk.Label(frame, text="Experiment:").grid(row=1, column=0, sticky="w",
                                                  pady=(6, 0))
        self.exp_var = tk.StringVar()
        self.exp_box = ttk.Combobox(frame, textvariable=self.exp_var,
                                    state="disabled")
        self.exp_box.grid(row=1, column=1, columnspan=2, sticky="we",
                          pady=(6, 0))
        self.exp_box.bind("<<ComboboxSelected>>",
                          lambda e: self._on_experiment_selected())
        self.exp_refresh_btn = ttk.Button(frame, text="Refresh",
                                          command=self._load_experiments_async,
                                          state="disabled")
        self.exp_refresh_btn.grid(row=1, column=3, sticky="e", padx=(8, 0),
                                  pady=(6, 0))

        ttk.Label(frame, text="Lua script:").grid(row=2, column=0, sticky="w",
                                                   pady=(6, 0))
        self.lua_script_var = tk.StringVar()
        self.lua_script_box = ttk.Combobox(
            frame, textvariable=self.lua_script_var, state="disabled")
        self.lua_script_box.grid(row=2, column=1, columnspan=2, sticky="we",
                                 pady=(6, 0))
        self.lua_script_box.bind(
            "<<ComboboxSelected>>", lambda e: self._on_lua_script_selected())
        self.lua_refresh_btn = ttk.Button(
            frame, text="Refresh", command=self._fetch_lua_catalog_async)
        self.lua_refresh_btn.grid(row=2, column=3, sticky="e", padx=(8, 0),
                                  pady=(6, 0))

        ttk.Label(frame, text="Wi-Fi SSID:").grid(row=3, column=0, sticky="w",
                                                  pady=(6, 0))
        self.ssid_var = tk.StringVar(value=self.settings.wifi_ssid)
        ttk.Entry(frame, textvariable=self.ssid_var, width=24).grid(
            row=3, column=1, sticky="w", pady=(6, 0))
        ttk.Label(frame, text="Password:").grid(row=3, column=2, sticky="e",
                                                pady=(6, 0))
        self.wifi_pass_var = tk.StringVar(value=self.settings.wifi_password)
        ttk.Entry(frame, textvariable=self.wifi_pass_var, show="•",
                  width=20).grid(row=3, column=3, sticky="we", pady=(6, 0))

    def _build_info_frame(self) -> None:
        frame = ttk.LabelFrame(self, text="Detected (read-only)", padding=8)
        frame.pack(fill="x", pady=(8, 0))
        self.clock_var = tk.StringVar()
        ttk.Label(frame, textvariable=self.clock_var).pack(anchor="w")
        tz_ok = timezones.firmware_supports(self.local_tz)
        tz_note = "" if tz_ok else ("  ⚠ not in the firmware's zone table — "
                                    "the device would reject it, so on-boarding "
                                    "is blocked (see the log)")
        self.tz_label = ttk.Label(
            frame, text=f"Timezone: {self.local_tz} "
                        f"({timezones.utc_offset_label()}){tz_note}",
            foreground=("black" if tz_ok else "#a15c00"))
        self.tz_label.pack(anchor="w")
        self.fw_var = tk.StringVar(value="Firmware release: fetching...")
        ttk.Label(frame, textvariable=self.fw_var).pack(anchor="w")
        self.lua_release_var = tk.StringVar(value="Lua release: fetching...")
        ttk.Label(frame, textvariable=self.lua_release_var).pack(anchor="w")

    def _build_device_frame(self) -> None:
        frame = ttk.LabelFrame(self, text="Device", padding=8)
        frame.pack(fill="both", expand=True, pady=(8, 0))
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="Serial port:").grid(row=0, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(frame, textvariable=self.port_var,
                                     state="readonly", width=44)
        self.port_box.grid(row=0, column=1, sticky="w")
        ttk.Button(frame, text="Refresh",
                   command=self._refresh_ports).grid(row=0, column=2,
                                                     padx=(8, 0))
        self.onboard_btn = ttk.Button(frame, text="On-board device",
                                      command=self._onboard)
        self.onboard_btn.grid(row=0, column=3, padx=(8, 0))

        steps_row = ttk.Frame(frame)
        steps_row.grid(row=1, column=0, columnspan=4, sticky="w", pady=(8, 0))
        self.step_labels: dict[str, ttk.Label] = {}
        for key, label in STEPS:
            lbl = ttk.Label(steps_row, text=f"· {label}", padding=(0, 0, 12, 0))
            lbl.pack(side="left")
            self.step_labels[key] = lbl

        btn_row = ttk.Frame(frame)
        btn_row.grid(row=2, column=0, columnspan=4, sticky="w", pady=(4, 0))
        self.retry_flash_btn = ttk.Button(
            btn_row, text="Retry flash", command=self._retry_flash,
            state="disabled")
        self.retry_flash_btn.pack(side="left")
        self.retry_prov_btn = ttk.Button(
            btn_row, text="Retry provisioning (no re-flash)",
            command=self._retry_provision, state="disabled")
        self.retry_prov_btn.pack(side="left", padx=(8, 0))
        self.result_var = tk.StringVar()
        ttk.Label(btn_row, textvariable=self.result_var,
                  font="TkHeadingFont").pack(side="left", padx=(16, 0))

        self.log_text = scrolledtext.ScrolledText(frame, height=14,
                                                  state="disabled",
                                                  font="TkFixedFont")
        self.log_text.grid(row=3, column=0, columnspan=4, sticky="nsew",
                           pady=(8, 0))
        frame.rowconfigure(3, weight=1)

    # ── thread-safe UI helpers ───────────────────────────────────────────
    def _post(self, fn, *args) -> None:
        """Schedule fn(*args) on the Tk main thread."""
        self.after(0, lambda: fn(*args))

    def log(self, msg: str) -> None:
        self._post(self._append_log, msg)

    def _append_log(self, msg: str) -> None:
        self.log_text.configure(state="normal")
        stamp = datetime.now().strftime("%H:%M:%S")
        self.log_text.insert("end", f"[{stamp}] {msg}\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _set_step(self, key: str, state: str) -> None:
        def apply():
            lbl = self.step_labels[key]
            text = dict(STEPS)[key]
            lbl.configure(text=f"{STEP_ICONS[state]} {text}",
                          foreground={"pending": "gray", "running": "#0a5",
                                      "ok": "green", "fail": "red"}[state])
        self._post(apply)

    def _reset_steps(self) -> None:
        for key, label in STEPS:
            self.step_labels[key].configure(text=f"· {label}",
                                            foreground="gray")
        self.result_var.set("")
        self.retry_flash_btn.configure(state="disabled")
        self.retry_prov_btn.configure(state="disabled")

    def _set_busy(self, busy: bool) -> None:
        def apply():
            self.busy = busy
            self.onboard_btn.configure(
                state="disabled" if busy else "normal")
            self.lua_script_box.configure(
                state="disabled" if busy or self.lua_catalog is None
                else "readonly")
            self.lua_refresh_btn.configure(
                state="disabled" if busy else "normal")
        self._post(apply)

    def _ask_on_main(self, fn):
        """Run fn() on the main thread, block the worker for its result."""
        answer: queue.Queue = queue.Queue(maxsize=1)
        self._post(lambda: answer.put(fn()))
        return answer.get()

    # ── clock ────────────────────────────────────────────────────────────
    def _tick_clock(self) -> None:
        utc = datetime.now(dt_timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        local = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self.clock_var.set(f"UTC: {utc}    Local: {local}")
        self.after(1000, self._tick_clock)

    # ── ports ────────────────────────────────────────────────────────────
    def _refresh_ports(self) -> None:
        ports = esptool_ops.list_serial_ports()
        display = []
        for p in ports:
            tag = "  [ambyte USB-JTAG]" if p["is_ambyte_jtag"] else ""
            display.append(f"{p['device']} — {p['description']}{tag}")
        self.port_box.configure(values=display)
        if display:
            # Preselect the first ambyte-looking port.
            self.port_box.current(0)
        else:
            self.port_var.set("")

    def _selected_port(self) -> str | None:
        value = self.port_var.get().strip()
        return value.split(" — ", 1)[0] if value else None

    # ── application + firmware releases ─────────────────────────────────
    def _fetch_gui_update_async(self) -> None:
        self.app_update_var.set("Checking for updates…")
        self.app_update_label.configure(foreground="#555555")
        self.app_check_btn.configure(state="disabled")

        def work():
            try:
                status = release_fetch.fetch_gui_update(
                    self.installed_gui, log=self.log
                )
            except release_fetch.ReleaseError as exc:
                self._post(self._apply_gui_update_error, str(exc))
                return
            self._post(self._apply_gui_update, status)

        threading.Thread(target=work, daemon=True).start()

    def _apply_gui_update(self, status: app_update.GuiUpdateStatus) -> None:
        self.gui_release_url = status.release_url
        if status.state == "current":
            text = f"UP TO DATE — {status.latest_version} is the latest release"
            colour = "green"
            button_text = "View this release"
        elif status.state == "outdated":
            text = (
                f"UPDATE AVAILABLE — download GUI {status.latest_version} "
                f"(running {status.installed.version})"
            )
            colour = "#b24a00"
            button_text = f"Download {status.latest_version}"
        elif status.state == "ahead":
            text = (
                f"PRE-RELEASE BUILD — latest published GUI is "
                f"{status.latest_version}"
            )
            colour = "#555555"
            button_text = "View latest release"
        elif self.is_packaged:
            self.app_version_var.set(
                "Running GUI version: UNKNOWN — update recommended"
            )
            text = (
                f"Latest available GUI is {status.latest_version}; this legacy "
                "build cannot be compared"
            )
            colour = "#b24a00"
            button_text = f"Download {status.latest_version}"
        else:
            text = (
                f"Latest downloadable GUI: {status.latest_version} "
                "(source run is not versioned)"
            )
            colour = "#555555"
            button_text = "View latest release"

        self.app_update_var.set(text)
        self.app_update_label.configure(foreground=colour)
        self.app_release_btn.configure(text=button_text, state="normal")
        self.app_check_btn.configure(state="normal")

    def _apply_gui_update_error(self, detail: str) -> None:
        self.gui_release_url = app_update.RELEASES_PAGE
        self.app_update_var.set(
            "UPDATE CHECK UNAVAILABLE — open Releases to verify manually"
        )
        self.app_update_label.configure(foreground="#b24a00")
        self.app_release_btn.configure(text="Open releases", state="normal")
        self.app_check_btn.configure(state="normal")
        self.log(f"GUI update check failed: {detail}")

    def _open_gui_release(self) -> None:
        webbrowser.open(self.gui_release_url)

    def _fetch_release_async(self) -> None:
        def work():
            try:
                release = release_fetch.fetch_latest(log=self.log)
                self.release = release
                self._post(self.fw_var.set,
                           f"Firmware release: {release.tag} (cached)")
            except release_fetch.ReleaseError as exc:
                self._post(self.fw_var.set, f"Firmware release: ERROR — {exc}")
                self.log(f"Firmware release fetch failed: {exc}")
        threading.Thread(target=work, daemon=True).start()

    def _fetch_lua_catalog_async(self) -> None:
        self.lua_catalog = None
        self.lua_script_box.configure(state="disabled", values=[])
        self.lua_script_var.set("")
        self.lua_release_var.set("Lua release: fetching...")
        self.lua_refresh_btn.configure(state="disabled")

        def work():
            try:
                catalog = release_fetch.fetch_latest_lua_catalog(log=self.log)
            except release_fetch.ReleaseError as exc:
                detail = str(exc)
                def show_error():
                    self.lua_release_var.set(f"Lua release: ERROR — {detail}")
                    if not self.busy:
                        self.lua_refresh_btn.configure(state="normal")
                self._post(show_error)
                self.log(f"Lua release fetch failed: {detail}")
                return

            def apply():
                self.lua_catalog = catalog
                values = [script.asset_name for script in catalog.scripts]
                self.lua_script_box.configure(
                    values=values,
                    state="disabled" if self.busy else "readonly")
                preferred = f"{self.settings.lua_script_name}.lua"
                selected = preferred if preferred in values else (
                    "main.lua" if "main.lua" in values else values[0])
                self.lua_script_var.set(selected)
                self._on_lua_script_selected()
                self.lua_release_var.set(
                    f"Lua release: {catalog.tag} ({len(values)} scripts)")
                if not self.busy:
                    self.lua_refresh_btn.configure(state="normal")

            self._post(apply)

        threading.Thread(target=work, daemon=True).start()

    def _selected_lua_script(self) -> release_fetch.LuaScriptRelease | None:
        if self.lua_catalog is None:
            return None
        selected = self.lua_script_var.get()
        return next(
            (script for script in self.lua_catalog.scripts
             if script.asset_name == selected), None)

    def _on_lua_script_selected(self) -> None:
        script = self._selected_lua_script()
        if script is None:
            return
        self.settings.lua_script_name = script.script_name
        self.settings.save()
        self.log(
            f"Selected Lua script: {script.asset_name} from {script.tag}.")

    # ── auth + experiments ───────────────────────────────────────────────
    def _env(self):
        return ENVIRONMENTS[self.env_var.get()]

    def _on_env_changed(self) -> None:
        self.settings.environment = self.env_var.get()
        self.settings.save()
        self.client = None
        self.user_label.set("not signed in")
        self.exp_box.configure(state="disabled", values=[])
        self.exp_var.set("")
        key = self.settings.api_key(self.env_var.get())
        if key:
            self._validate_key_async(key, on_startup=True)

    def _sign_in(self) -> None:
        env = self._env()
        webbrowser.open(env.api_keys_url)
        key = simpledialog.askstring(
            "openJII API key",
            f"A browser window opened at:\n{env.api_keys_url}\n\n"
            "Sign in there, create an API key (it is shown once),\n"
            "and paste it here (jii_...):",
            parent=self)
        if key and key.strip():
            self._validate_key_async(key.strip())

    def _validate_key_async(self, key: str, on_startup: bool = False) -> None:
        env = self._env()

        def work():
            client = OpenJIIClient(env, key)
            try:
                user = client.validate_key()
            except OpenJIIError as exc:
                if on_startup:
                    self.log(f"Stored API key for '{env.key}' is no longer "
                             f"valid: {exc}")
                else:
                    self._post(messagebox.showerror, "openJII sign-in", str(exc))
                return
            self.client = client
            self.settings.set_api_key(env.key, key)
            self.settings.save()
            who = user.get("email") or user.get("name") or "signed in"
            self._post(self.user_label.set, f"{env.key}: {who}")
            self.log(f"openJII: signed in to {env.key} as {who}.")
            self._load_experiments_async()
        threading.Thread(target=work, daemon=True).start()

    def _load_experiments_async(self) -> None:
        if not self.client:
            return

        def work():
            try:
                exps = self.client.list_experiments()
            except OpenJIIError as exc:
                self.log(f"Loading experiments failed: {exc}")
                self._post(messagebox.showerror, "openJII experiments", str(exc))
                return
            self.experiments = exps
            names = [f"{e.name}  ({e.id})" for e in exps]

            def apply():
                self.exp_box.configure(state="readonly", values=names)
                self.exp_refresh_btn.configure(state="normal")
                # Restore the session's previous selection when still present.
                prev = self.settings.experiment_id
                for i, e in enumerate(exps):
                    if e.id == prev:
                        self.exp_box.current(i)
                        self._on_experiment_selected()
                        break
            self._post(apply)
            self.log(f"Loaded {len(exps)} active experiment(s).")
        threading.Thread(target=work, daemon=True).start()

    def _current_experiment(self):
        sel = self.exp_var.get()
        for e in self.experiments:
            if f"({e.id})" in sel:
                return e
        return None

    def _on_experiment_selected(self) -> None:
        exp = self._current_experiment()
        if not exp:
            return
        self.settings.experiment_id = exp.id
        self.settings.experiment_name = exp.name
        self.settings.save()

    # ── the procedure ────────────────────────────────────────────────────
    def _session_ready(self) -> str | None:
        """None when ready, else the reason the procedure cannot start."""
        if self.busy:
            return "a procedure is already running."
        if not self.client:
            return "sign in to openJII first (API key)."
        if self.release is None:
            return "the firmware release is not available yet."
        if self._selected_lua_script() is None:
            return "the Lua release or script selection is not available yet."
        if not self._current_experiment():
            return "select an experiment first."
        if not self.ssid_var.get().strip():
            return "enter the Wi-Fi SSID the boards should join."
        if not self._selected_port():
            return "select a serial port."
        if not timezones.firmware_supports(self.local_tz):
            # `cfg set timezone` fails closed on an unknown zone, so this would
            # otherwise be flashed and then fail verification unrepairably.
            return (f"this PC's timezone '{self.local_tz}' is not in the "
                    f"firmware's zone table (IANA tzdata "
                    f"{timezones.FIRMWARE_TZDATA_VERSION}). Set the PC to a "
                    "standard IANA zone, or regenerate the table with "
                    "tools/gen_tz_table.py and release firmware built from it.")
        return None

    def _make_context(self) -> SessionContext:
        self.settings.wifi_ssid = self.ssid_var.get().strip()
        self.settings.wifi_password = self.wifi_pass_var.get()
        self.settings.save()
        lua_script = self._selected_lua_script()
        assert lua_script is not None, "_session_ready must validate the Lua selection"
        experiment = self._current_experiment()
        assert experiment is not None, "_session_ready must validate the experiment selection"
        return SessionContext(
            env=self._env(),
            client=self.client,
            release=self.release,
            lua_script=lua_script,
            experiment_id=experiment.id,
            timezone=self.local_tz,
            wifi_ssid=self.settings.wifi_ssid,
            wifi_password=self.settings.wifi_password,
        )

    def _onboard(self) -> None:
        reason = self._session_ready()
        if reason:
            messagebox.showwarning("Not ready", f"Cannot start: {reason}")
            return
        port = self._selected_port()
        ctx = self._make_context()
        self._reset_steps()
        self._set_busy(True)
        threading.Thread(target=self._run_procedure, args=(ctx, port),
                         daemon=True).start()

    def _run_procedure(self, ctx: SessionContext, port: str) -> None:
        try:
            # 1. pre-flash check
            self._set_step("check", "running")
            info = procedure.preflight(port, log=self.log)
            self._set_step("check", "ok")

            # name prompt (main thread)
            def ask() -> str | None:
                return simpledialog.askstring(
                    "Device name",
                    f"Board MAC: {info.mac}\n\nDevice name:",
                    initialvalue=info.proposed_name, parent=self)
            name = self._ask_on_main(ask)
            if name is None:
                self.log("On-boarding cancelled at the name prompt.")
                self._set_step("check", "pending")
                return
            while clean_device_name(name) is None:
                bad = name

                def ask_again() -> str | None:
                    return simpledialog.askstring(
                        "Device name",
                        f"'{bad}' is invalid (max 63 chars, printable ASCII, "
                        "no quotes/backslashes).\n\nDevice name:",
                        initialvalue=info.proposed_name, parent=self)
                name = self._ask_on_main(ask_again)
                if name is None:
                    self.log("On-boarding cancelled at the name prompt.")
                    self._set_step("check", "pending")
                    return

            run = DeviceRun(port=port, preflight=info, name=name)
            self.current_run = run

            # 2-3. credentials + NVS
            self._set_step("credentials", "running")
            procedure.prepare_provisioning(ctx, run, log=self.log)
            self._set_step("credentials", "ok")
            self._set_step("nvs", "ok")

            self._do_flash_and_verify(ctx, run)
        except ProcedureError as exc:
            self._fail_step(exc)
        except Exception as exc:   # anything unexpected must surface, not hang
            self.log(f"UNEXPECTED ERROR: {exc!r}")
            self._post(messagebox.showerror, "On-boarding failed", str(exc))
        finally:
            self._set_busy(False)

    def _do_flash_and_verify(self, ctx: SessionContext, run: DeviceRun) -> None:
        # 4. flash
        self._set_step("flash", "running")
        procedure.flash(ctx, run, log=self.log)
        self._set_step("flash", "ok")
        self._verify(ctx, run)

    def _verify(self, ctx: SessionContext, run: DeviceRun,
                repair: bool = False) -> None:
        # 5-6. RTC + verification
        self._set_step("provision", "running")
        if repair:
            results = procedure.provision_and_verify_with_repair(
                ctx, run, log=self.log)
        else:
            results = procedure.provision_and_verify(ctx, run, log=self.log)
        self._set_step("provision", "ok")

        config_ok = all(r.passed for r in results)
        if config_ok:
            self._set_step("lua", "running")
            results.append(procedure.install_lua_script(ctx, run, log=self.log))
            self._set_step("lua", "ok")

        all_ok = config_ok and all(r.passed for r in results)
        self._set_step("verify", "ok" if all_ok else "fail")
        summary = "   ".join(
            f"{r.label}: {'PASS' if r.passed else 'FAIL'}" for r in results)
        self._post(self.result_var.set, summary)
        if all_ok:
            self.log(f"DONE — {run.name} on-boarded and verified. "
                     "Unplug it and connect the next board.")
        else:
            failed = ", ".join(r.label for r in results if not r.passed)
            self.log(f"Verification FAILED for: {failed}. Use 'Retry "
                     "provisioning' — no re-flash needed.")
            self._post(self.retry_prov_btn.configure, {"state": "normal"})

    def _fail_step(self, exc: ProcedureError) -> None:
        self._set_step(exc.step, "fail")
        self.log(f"FAILED at {exc.step}: {exc}")
        if exc.step == "flash":
            self.log("The board is still in a re-flashable state. Fix the "
                     "connection and click 'Retry flash'.")
            self._post(self.retry_flash_btn.configure, {"state": "normal"})
        elif exc.step in ("provision", "lua", "verify"):
            self._post(self.retry_prov_btn.configure, {"state": "normal"})
        self._post(messagebox.showerror, "On-boarding failed", str(exc))

    def _retry_flash(self) -> None:
        run = self.current_run
        if not run or not run.nvs_path or self.busy:
            return
        ctx = self._make_context()
        self._set_busy(True)
        self._post(self.retry_flash_btn.configure, {"state": "disabled"})

        def work():
            try:
                self._do_flash_and_verify(ctx, run)
            except ProcedureError as exc:
                self._fail_step(exc)
            except Exception as exc:
                self.log(f"UNEXPECTED ERROR: {exc!r}")
            finally:
                self._set_busy(False)
        threading.Thread(target=work, daemon=True).start()

    def _retry_provision(self) -> None:
        run = self.current_run
        if not run or not run.plan or self.busy:
            return
        ctx = self._make_context()
        self._set_busy(True)
        self._post(self.retry_prov_btn.configure, {"state": "disabled"})

        def work():
            try:
                self._verify(ctx, run, repair=True)
            except ProcedureError as exc:
                self._fail_step(exc)
            except Exception as exc:
                self.log(f"UNEXPECTED ERROR: {exc!r}")
            finally:
                self._set_busy(False)
        threading.Thread(target=work, daemon=True).start()


def main() -> None:
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    apply_ui_scale(root)
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()

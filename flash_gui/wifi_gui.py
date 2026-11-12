# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Wi-Fi maintenance window; all USB work stays off the Tk thread."""
import threading
import tkinter as tk
from tkinter import messagebox, ttk

from . import wifi_settings


class WifiDialog(tk.Toplevel):
    def __init__(self, app, port):
        super().__init__(app)
        try:
            self._build(app, port)
        except Exception:
            # __init__ can fail after Tk has registered the window (including
            # a failed modal grab). Release that partial Toplevel as well as
            # the caller's busy gate; do not strand an orphaned modal window.
            self.destroy()
            raise

    def _build(self, app, port):
        self.app, self.port = app, port
        self.info = None
        self.working = False
        self.allow_new = False
        self.saved_schedule = None
        self.title("Wi-Fi settings (USB)")
        self.transient(app.winfo_toplevel())
        self.protocol("WM_DELETE_WINDOW", self._close)
        frame = ttk.Frame(self, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        ttk.Label(frame, text="Update the connected Ambyte's Wi-Fi locally.\n"
                  "No openJII sign-in, experiment selection or firmware download required.",
                  wraplength=640).grid(row=0, column=0, columnspan=2, sticky="w")
        self.device_text = tk.StringVar(value=f"Serial port: {port}\nReading device…")
        ttk.Label(frame, textvariable=self.device_text, wraplength=640,
                  font="TkFixedFont").grid(row=1, column=0, columnspan=2, sticky="w", pady=12)
        self.ssid = tk.StringVar(value=app.ssid_var.get())
        # Local edits live only in this window, never Settings.save or an NVS
        # image. Keep it open to service the next board with the same hub.
        self.password = tk.StringVar()
        ttk.Label(frame, text="Wi-Fi SSID:").grid(row=2, column=0, sticky="w", padx=(0, 10))
        self.ssid_entry = ttk.Entry(frame, textvariable=self.ssid, width=38)
        self.ssid_entry.grid(row=2, column=1, sticky="ew")
        ttk.Label(frame, text="Password:").grid(row=3, column=0, sticky="w", pady=(8, 0))
        self.password_entry = ttk.Entry(frame, textvariable=self.password, show="•", width=38)
        self.password_entry.grid(row=3, column=1, sticky="ew", pady=(8, 0))
        self.network_note = tk.StringVar(value="Leave the password blank for an open network. Choose the correct hub for this board.")
        ttk.Label(frame, textvariable=self.network_note,
                  wraplength=640).grid(row=4, column=0, columnspan=2, sticky="w", pady=10)
        buttons = ttk.Frame(frame)
        buttons.grid(row=5, column=0, columnspan=2, sticky="w")
        self.read_btn = ttk.Button(buttons, text="Read device again", command=self._read)
        self.read_btn.pack(side="left")
        self.update_btn = ttk.Button(buttons, text="Save Wi-Fi", command=self._update)
        self.update_btn.pack(side="left", padx=(8, 0))
        self.test_btn = ttk.Button(buttons, text="Test & save", command=lambda: self._update(test_connection=True))
        self.test_btn.pack(side="left", padx=(8, 0))
        self.recover_btn = ttk.Button(buttons, text="Check saved result", command=self._recover)
        self.recover_btn.pack(side="left", padx=(8, 0))
        self.close_btn = ttk.Button(buttons, text="Close", command=self._close)
        self.close_btn.pack(side="left", padx=(8, 0))
        self.new_btn = ttk.Button(frame, text="Start new Wi-Fi attempt…", command=self._new_attempt)
        self.new_btn.grid(row=6, column=0, columnspan=2, sticky="w", pady=(8, 0))
        self.new_btn.grid_remove()
        self.message = tk.StringVar()
        ttk.Label(frame, textvariable=self.message, wraplength=640).grid(
            row=7, column=0, columnspan=2, sticky="w", pady=12)
        ttk.Label(frame, text="Firmware, cloud identity, certificates and measurement schedule are preserved.",
                  wraplength=640).grid(row=8, column=0, columnspan=2, sticky="w")
        self.grab_set()
        self._read()

    def _controls(self):
        state = "disabled" if self.working else "normal"
        for widget in (self.read_btn, self.close_btn, self.ssid_entry, self.password_entry):
            widget.configure(state=state)
        self.update_btn.configure(state="normal" if not self.working and self.info else "disabled")
        self.test_btn.configure(state="normal" if not self.working and self.info and self.info.service else "disabled")
        self.recover_btn.configure(state="normal" if not self.working and self.info and self.info.service else "disabled")
        self.new_btn.configure(state="normal" if not self.working and self.info and (self.allow_new or self.saved_schedule) else "disabled")
        self.new_btn.configure(text=("Cancel staged schedule…" if self.saved_schedule and self.saved_schedule.action == "cancel"
                                    else "Archive unresolved schedule…" if self.saved_schedule else "Start new Wi-Fi attempt…"))
        if (self.allow_new or self.saved_schedule) and self.info and self.info.service:
            self.new_btn.grid()
        else:
            self.new_btn.grid_remove()

    def _start(self, action, apply, progress, *, service_recovery=True):
        if self.working:
            return
        self.working = True
        self.message.set(progress)
        self._controls()
        def finish(value, error):
            if not self.winfo_exists():
                return
            self.working = False
            if error:
                self.allow_new = error.uncertain and service_recovery
                self.saved_schedule = None
                self.message.set(str(error))
            else:
                apply(value)
            self._controls()
        def work():
            try:
                value, error = action(), None
            except wifi_settings.WifiSettingsError as exc:
                value, error = None, exc
            except Exception:
                # Serial exceptions can include command bytes. Keep unexpected
                # failures generic; no raw serial capture is logged here.
                value, error = None, wifi_settings.WifiSettingsError(
                    "Could not communicate with the Ambyte. Check USB, close other serial tools, then read the device again.")
            try:
                self.app._post(finish, value, error)
            except (RuntimeError, tk.TclError):
                pass  # App closure is allowed; the worker closes its USB handle.
        threading.Thread(target=work, daemon=True).start()

    def _read(self):
        if self.working:
            return
        previous = self.info
        self.info = None  # A failed refresh must not leave stale write permission.
        self.saved_schedule = None
        self.device_text.set(f"Serial port: {self.port}\nReading device…")
        def apply(info):
            if previous is None or previous.mac != info.mac:
                self.allow_new = False
            self.info = info
            state = {True: "connected", False: "disconnected", None: "unavailable"}[info.connected]
            self.device_text.set(f"Serial port: {info.port}\nDevice name: {info.name}\n"
                                 f"MAC: {info.mac}\nFirmware: {info.firmware or 'unavailable (older firmware)'}\nWi-Fi at last read: {state}")
            behavior = ("Save Wi-Fi replaces the stored credentials even if the hub is out of range. "
                        "Check the password before saving. "
                        + ("Optional Test & save requires the hub in range and saves only after a successful connection."
                           if info.service else "Connection testing with rollback is unavailable on this older firmware."))
            self.network_note.set("Leave the password blank for an open network.\n" + behavior)
            self.message.set("Check the device name and MAC before updating.")
        self._start(lambda: wifi_settings.read_device(self.port), apply,
                    "Reading the Ambyte on the selected USB port; startup can take 45 seconds…")

    def _update(self, *, new_request=False, test_connection=False):
        if self.working or self.info is None or (test_connection and not self.info.service):
            return
        ssid, password, info = self.ssid.get(), self.password.get(), self.info
        try:
            wifi_settings.validate_credentials(ssid, password)
        except wifi_settings.WifiSettingsError as exc:
            self.message.set(str(exc))
            return
        def apply(result):
            self.allow_new = False
            self.saved_schedule = None
            if result.connected:
                self.message.set(f"Wi-Fi saved and connection confirmed for {result.ssid} on {info.mac}.")
            else:
                self.message.set(f"Wi-Fi saved for {result.ssid} on {info.mac}. Connection not confirmed; "
                                 "power-cycle at deployment if it does not connect when the hub is available.")
        action = (lambda: wifi_settings.update_wifi(info, ssid, password, new_request=new_request)) if test_connection else (
            lambda: wifi_settings.save_wifi(info, ssid, password))
        self._start(action, apply,
                    f"Checking {info.mac} and waiting for startup (up to 3 minutes), then "
                    + ("testing Wi-Fi; keep the hub in range and USB connected…" if test_connection else
                       "saving Wi-Fi; a busy driver can take up to 90 seconds. The hub can be out of range. Keep USB connected…"),
                    service_recovery=test_connection)

    def _recover(self):
        if self.working or self.info is None:
            return
        info = self.info
        def apply(result):
            self.allow_new = False
            self.saved_schedule = None
            if isinstance(result, wifi_settings.SavedSchedule):
                self.saved_schedule = result if result.action else None
                self.message.set(result.message)
                return
            self.message.set(f"Earlier saved result for {info.mac}: Wi-Fi {result.ssid} was saved and its connection was confirmed then. "
                             "Use Read device again to check the current connection.")
        self._start(lambda: wifi_settings.check_saved_result(info), apply,
                    "Reading the saved result without resending the password…")

    def _new_attempt(self):
        if self.working or self.info is None:
            return
        if self.saved_schedule:
            saved, info = self.saved_schedule, self.info
            verb = "Cancel the staged transfer" if saved.action == "cancel" else "Archive the unresolved recovery record"
            if messagebox.askyesno("Saved schedule recovery",
                    f"{verb} for schedule {saved.op}, version {saved.version}, on {info.mac}?\n\n"
                    "This does not install a new workbook. Archiving saves this record on disk and replaces the single previous archive.", parent=self):
                def apply(result):
                    self.saved_schedule = result if result.action else None
                    self.message.set(result.message)
                self._start(lambda: wifi_settings.resolve_saved_schedule(info, saved), apply,
                            "Rechecking the saved schedule before resolving its recovery record…")
            return
        if not self.allow_new:
            return
        if messagebox.askyesno("New Wi-Fi attempt",
                f"Start a new Wi-Fi attempt for {self.info.name} ({self.info.mac})?\n\n"
                "The previous result could not be confirmed. Check saved result first. "
                "A new attempt sends the currently entered credentials again; the device will refuse if another operation is still active.",
                parent=self):
            self._update(new_request=True, test_connection=True)

    def _close(self):
        if self.working:
            return
        self.password.set("")
        self.grab_release()
        self.destroy()
        self.app._set_busy(False)

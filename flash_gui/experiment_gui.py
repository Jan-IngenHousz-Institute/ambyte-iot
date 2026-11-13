# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Device destination and experiment-only maintenance, separate from provisioning."""
from dataclasses import replace
import threading
import tkinter as tk
from tkinter import ttk, messagebox

from . import experiment_settings as settings, maintenance, location_settings


class ExperimentDialog(tk.Toplevel):
    def __init__(self, app, port):
        super().__init__(app)
        try:
            self.app, self.port, self.target, self.working = app, port, None, False
            self.title('Device destination / change experiment')
            self.transient(app.winfo_toplevel())
            self.protocol('WM_DELETE_WINDOW', self._close)
            frame = ttk.Frame(self, padding=12)
            frame.pack(fill='both', expand=True)
            self.summary = tk.StringVar(value='Reading the connected device…')
            ttk.Label(frame, textvariable=self.summary, wraplength=720, font='TkFixedFont').pack(anchor='w')
            ttk.Label(frame, text='The API environment selects your account and workbook access. It does not change the device broker.\n'
                      'Certificate presence and validity cannot be read on this firmware. Cloud status below is registry evidence only.',
                      wraplength=720).pack(anchor='w', pady=12)
            self.cloud = tk.StringVar(value='Cloud status: not checked')
            ttk.Label(frame, textvariable=self.cloud, wraplength=720).pack(anchor='w')
            row = ttk.Frame(frame); row.pack(anchor='w', pady=12)
            self.read_btn = ttk.Button(row, text='Read device destination', command=self._read)
            self.read_btn.pack(side='left')
            self.cloud_btn = ttk.Button(row, text='Check cloud status', command=self._cloud)
            self.cloud_btn.pack(side='left', padx=8)
            self.change_btn = ttk.Button(row, text='Change experiment / workbook…', command=self._change)
            self.change_btn.pack(side='left')
            self.close_btn = ttk.Button(row, text='Close', command=self._close)
            self.close_btn.pack(side='left', padx=8)
            self.maintenance_text = tk.StringVar(value="Workbook: waiting for device read")
            ttk.Label(frame, textvariable=self.maintenance_text, wraplength=720).pack(anchor="w")
            from .location_gui import LocationPicker
            self.current_location = LocationPicker(frame, "Current experiment location:", self._controls)
            self.current_location.pack(fill='x', pady=8)
            self.location_btn = ttk.Button(frame, text='Set location from experiment', command=self._set_location)
            self.location_btn.pack(anchor='w')
            self.destination_location = LocationPicker(frame, 'Destination experiment location:', self._location_default)
            self.destination_location.pack(fill='x', pady=8)
            self.also_location = tk.BooleanVar(value=False)
            self.also_text = tk.StringVar(value="Also set this device's location")
            self.also_btn = ttk.Checkbutton(frame, textvariable=self.also_text, variable=self.also_location)
            self.also_btn.pack(anchor='w')
            self.location_default_reason = tk.StringVar()
            ttk.Label(frame, textvariable=self.location_default_reason, wraplength=720).pack(anchor='w')
            self.location_key = None
            self.location_loading = False
            self.message = tk.StringVar()
            ttk.Label(frame, textvariable=self.message, wraplength=720).pack(anchor='w')
            self.grab_set()
            self._read()
        except Exception:
            self.destroy()
            raise

    def _controls(self):
        self.read_btn.configure(state='disabled' if self.working else 'normal')
        self.close_btn.configure(state='disabled' if self.working else 'normal')
        ready = not self.working and self.target is not None and self.app.client is not None
        self.cloud_btn.configure(state='normal' if ready else 'disabled')
        self.change_btn.configure(state='normal' if ready and self.app._current_experiment() is not None
                                  and not getattr(self, 'location_loading', False) else 'disabled')
        if hasattr(self, 'location_btn'):
            self.location_btn.configure(state='normal' if ready and self.current_location.selected() else 'disabled')
            self.also_btn.configure(state='normal' if ready and self.destination_location.selected() else 'disabled')

    def _run(self, action, apply, progress, *, maintain=False, after_primary=None):
        if self.working:
            return
        self.working = True
        self._last_logged_timezone_result = None
        self.message.set(progress)
        self._controls()
        client = self.app.client
        def primary_ready(value):
            if self.winfo_exists():
                apply(value)
                self.maintenance_text.set('Checking workbook and fallback timezone… keep USB connected. A workbook install or reboot may be needed.')

        def finish(value, error, extra):
            if not self.winfo_exists():
                return
            self.working = False
            if error:
                self.target = None
                self.message.set(error)
            else:
                apply(value)
                if extra is not None:
                    self.maintenance_text.set(extra.text)
            self._controls()
        def work():
            extra = None
            try:
                value, error = action(), None
            except Exception as exc:
                value, error = None, str(exc)
            if error is None and (maintain or after_primary is not None):
                try:
                    self.app._post(primary_ready, value)
                    if after_primary is not None:
                        value = after_primary(value)
                    extra = maintenance.run(value.device.port, value.mac, client,
                        confirm_timezone=(None if after_primary is not None else
                            lambda status: self.app._confirm_timezone(status, parent=self)), log=self.app.log)
                    refreshed = settings.read_target(extra.port or value.device.port,
                        dict(getattr(self.app.settings, "mqtt_endpoints", {})))
                    value = replace(refreshed, timezone_result=value.timezone_result, location_result=value.location_result)
                except settings.ExperimentSettingsError:
                    pass  # A readback failure does not erase the primary result.
                except Exception:
                    extra = maintenance.MaintenanceResult('Workbook: not checked: optional maintenance unavailable',
                                                          'Fallback timezone: not checked')
            try:
                self.app._post(finish, value, error, extra)
            except (RuntimeError, tk.TclError):
                pass
        threading.Thread(target=work, daemon=True).start()

    def _show(self, target):
        self.target, self.port = target, target.device.port
        identity = target.device.identity
        self.summary.set(f'MAC: {target.mac}\nDevice: {identity.thing_name}\nFirmware: {identity.fw}\n'
                         f'MQTT broker: {target.broker}\nBroker environment: {target.environment or "Unknown / unverified"}\n'
                         f'Current experiment: {target.experiment_id or "Unrecognized topic"}\n'
                         f'Workbook version: {identity.schedule.workbook_version_id or "None reported"}')
        self.cloud.set('Cloud status: not checked. Device certificate remains unverified.')
        experiment = self.app._current_experiment()
        if self.app.client is None or experiment is None:
            self.message.set('Close this window, sign in and select the destination experiment in the main window, then reopen Change experiment / workbook.')
        else:
            self.message.set(f'Selected destination: {experiment.name} ({experiment.id}), API environment {self.app.client.env.key}.')
        if target.location_result:
            self.message.set(self.message.get() + '\n' + target.location_result)
        if hasattr(self, 'current_location'):
            self._load_locations(target)
        if target.timezone_result:
            self.message.set(self.message.get() + '\n' + target.timezone_result)
            if self.__dict__.get('_last_logged_timezone_result') != target.timezone_result:
                self.app.log(target.timezone_result)
                self._last_logged_timezone_result = target.timezone_result


    def _load_locations(self, target):
        experiment = self.app._current_experiment()
        client = self.app.client
        key = (target.mac, target.experiment_id, experiment.id if experiment else None, id(client))
        if key == self.location_key:
            return
        self.location_key, self.location_loading = key, True
        pending = location_settings.LocationOptions(reason='Reading experiment locations…')
        self.current_location.set_options(pending)
        self.destination_location.set_options(pending)
        def finish(current, destination):
            if not self.winfo_exists() or self.location_key != key:
                return
            self.location_loading = False
            self.current_location.set_options(current)
            self.destination_location.set_options(destination)
            self._location_default()
            self._controls()
        def work():
            current = location_settings.current_options(target, client)
            destination = (location_settings.destination_options(client, experiment.id) if client and experiment else
                           location_settings.LocationOptions(reason='Select a destination experiment.'))
            self.app._post(finish, current, destination)
        threading.Thread(target=work, daemon=True).start()

    def _location_default(self):
        selected = self.destination_location.selected()
        default, reason = (location_settings.change_default(self.target.device.identity, selected)
                           if selected and self.target else (False, 'Choose a destination location first.'))
        self.also_location.set(default)
        self.also_text.set("Also set this device's location to " + (selected.name if selected else 'the selected site'))
        self.location_default_reason.set(reason)
        self._controls()

    def _confirm_location(self, prompt):
        return self.app._ask_on_main(lambda: messagebox.askyesno('Set experiment location', prompt, parent=self))

    def _set_location(self):
        selected = self.current_location.selected()
        if self.working or self.target is None or selected is None:
            return
        target = self.target
        def apply(result):
            self.message.set(result)  # Operator-facing coordinates, never app.log.
            self.target = None  # Require a fresh read after any possible write.
        self._run(lambda: location_settings.set_location(target, selected,
                  confirm=self._confirm_location, log=self.app.log), apply,
                  'Saving experiment location, then rebooting and verifying the same Ambyte…')

    def _read(self):
        if self.working:
            return
        self.target = None
        if hasattr(self, 'current_location'):
            self.location_key = None
        endpoints = dict(getattr(self.app.settings, 'mqtt_endpoints', {}))
        self._run(lambda: settings.read_target(self.port, endpoints), self._show,
                  'Reading identity, broker and topics. The following workbook check may install or reboot; keep USB connected…', maintain=True)

    def _cloud(self):
        if self.working or self.target is None or self.app.client is None:
            return
        target, client = self.target, self.app.client
        self._run(lambda: settings.cloud_status(target, client), self.cloud.set,
                  'Reading registry status; registration and credentials are preserved…')

    def _change(self):
        if self.working or self.target is None or self.app.client is None:
            return
        experiment = self.app._current_experiment()
        if experiment is None:
            return
        target, client = self.target, self.app.client
        if target.environment and target.environment != client.env.key:
            self.message.set('The device broker and selected API environment differ. Select the matching API environment; this action preserves device credentials.')
            return
        unknown = ('\nBroker environment is UNKNOWN: confirm that this existing broker is correct for the selected API environment. '
                   'Credential validity has not been checked.\n' if not target.environment else '')
        if not messagebox.askyesno('Change experiment / workbook',
                f'Device: {target.mac}\nKeep broker: {target.broker}\nAPI environment: {client.env.key}\n'
                f'Destination: {experiment.name} ({experiment.id})\n{unknown}\n'
                'Install its workbook and change the data/status topics, keeping registration and certificates. '
                'Queued and transitional records follow the topic/workbook current when published. Continue?', parent=self):
            return
        def apply(value):
            self._show(value)
            self.message.set('Experiment topics and workbook verified after reboot. Broker and client identity preserved; no registration or certificate writes.'
                             + ('\n' + value.timezone_result if value.timezone_result else '')
                             + ('\n' + value.location_result if value.location_result else ''))
        selected = (self.destination_location.selected() if hasattr(self, 'destination_location')
                    and self.also_location.get() else None)
        def set_destination_location(value):
            result = location_settings.set_location(value, selected, confirm=self._confirm_location, log=self.app.log)
            return replace(value, location_result=result)
        self._run(lambda: settings.change_experiment(target, client, experiment.id,
                  allow_unknown_environment=not bool(target.environment), log=self.app.log), apply,
                  'Installing the destination workbook, saving topics, then rebooting and verifying the same Ambyte…', maintain=True,
                  after_primary=set_destination_location if selected else None)

    def _close(self):
        if self.working:
            return
        self.grab_release()
        self.destroy()
        self.app._set_busy(False)

# SPDX-FileCopyrightText: 2026 Jan Ingenhousz Institute
# SPDX-License-Identifier: GPL-3.0-only
"""Device destination and experiment-only maintenance, separate from provisioning."""
import threading
import tkinter as tk
from tkinter import ttk, messagebox

from . import experiment_settings as settings


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
        self.change_btn.configure(state='normal' if ready and self.app._current_experiment() is not None else 'disabled')

    def _run(self, action, apply, progress):
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
                self.target = None
                self.message.set(error)
            else:
                apply(value)
            self._controls()
        def work():
            try:
                value, error = action(), None
            except Exception as exc:
                value, error = None, str(exc)
            try:
                self.app._post(finish, value, error)
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

    def _read(self):
        if self.working:
            return
        self.target = None
        endpoints = dict(getattr(self.app.settings, 'mqtt_endpoints', {}))
        self._run(lambda: settings.read_target(self.port, endpoints), self._show,
                  'Reading identity, broker and topics without changing configuration…')

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
            self.message.set('Experiment topics and workbook verified after reboot. Broker and client identity preserved; no registration or certificate writes.')
        self._run(lambda: settings.change_experiment(target, client, experiment.id,
                  allow_unknown_environment=not bool(target.environment), log=self.app.log), apply,
                  'Installing the destination workbook, saving topics, then rebooting and verifying the same Ambyte…')

    def _close(self):
        if self.working:
            return
        self.grab_release()
        self.destroy()
        self.app._set_busy(False)

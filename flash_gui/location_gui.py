# SPDX-License-Identifier: GPL-3.0-only
"""Shared explicit location choice. Several sites never acquire a silent default."""
import tkinter as tk
from tkinter import ttk


class LocationPicker(ttk.Frame):
    def __init__(self, parent, title, on_select=lambda: None):
        super().__init__(parent)
        self.options = ()
        self.value = tk.StringVar()
        self.reason = tk.StringVar(value='Reading experiment locations…')
        ttk.Label(self, text=title).pack(anchor='w')
        self.box = ttk.Combobox(self, textvariable=self.value, state='disabled', width=65)
        self.box.pack(fill='x')
        self.box.bind('<<ComboboxSelected>>', lambda event: on_select())
        ttk.Label(self, textvariable=self.reason, wraplength=720).pack(anchor='w')

    def set_options(self, result):
        self.options = result.locations
        # Include IDs to distinguish otherwise identical names/coordinates.
        labels = [f'{item.label} [{item.id}]' for item in self.options]
        self.box.configure(values=labels, state='readonly' if labels else 'disabled')
        self.value.set(labels[0] if len(labels) == 1 else '')
        self.reason.set(result.reason or ('Choose a location explicitly.' if len(labels) > 1 else ''))

    def selected(self):
        value = self.value.get()
        return next((item for item in self.options if value == f'{item.label} [{item.id}]'), None)

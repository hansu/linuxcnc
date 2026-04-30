#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Minimal HAL show skeleton using GTK3.

This script creates a split GTK window with a tree view on the left and a details
panel on the right. It queries the LinuxCNC HAL subsystem using the external
`hal` command and displays component/pin/param/sig lists.

This is a first step skeleton; it does not yet implement editing or a full
watch mode.
"""

import subprocess
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk


HAL_GROUPS = [
    ("comp", "Components"),
    ("pin", "Pins"),
    ("param", "Parameters"),
    ("sig", "Signals"),
    ("funct", "Functions"),
    ("thread", "Threads"),
]


class HalShowWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="HAL Show (GTK)")
        self.set_default_size(900, 600)
        self.connect("destroy", Gtk.main_quit)

        main_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        self.add(main_box)

        toolbar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        refresh_button = Gtk.Button(label="Refresh")
        refresh_button.connect("clicked", lambda _btn: self.populate_tree())
        toolbar.pack_start(refresh_button, False, False, 0)

        main_box.pack_start(toolbar, False, False, 0)

        paned = Gtk.Paned.new(Gtk.Orientation.HORIZONTAL)
        main_box.pack_start(paned, True, True, 0)

        self.tree_store = Gtk.TreeStore(str, str, str, str)
        self.tree_view = Gtk.TreeView(model=self.tree_store)
        tree_column = Gtk.TreeViewColumn("HAL Tree")
        cell_renderer = Gtk.CellRendererText()
        tree_column.pack_start(cell_renderer, True)
        tree_column.add_attribute(cell_renderer, "text", 0)
        self.tree_view.append_column(tree_column)
        self.tree_view.set_enable_tree_lines(True)
        self.tree_view.set_activate_on_single_click(True)
        self.tree_view.get_selection().connect("changed", self.on_tree_selection_changed)

        tree_scroll = Gtk.ScrolledWindow()
        tree_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        tree_scroll.add(self.tree_view)

        detail_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        detail_label = Gtk.Label(label="Details")
        detail_label.set_xalign(0.0)
        detail_box.pack_start(detail_label, False, False, 0)

        self.detail_textview = Gtk.TextView()
        self.detail_textview.set_editable(False)
        self.detail_textview.set_cursor_visible(False)
        self.detail_buffer = self.detail_textview.get_buffer()
        detail_scroll = Gtk.ScrolledWindow()
        detail_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        detail_scroll.add(self.detail_textview)
        detail_box.pack_start(detail_scroll, True, True, 0)

        paned.add1(tree_scroll)
        paned.add2(detail_box)
        paned.set_position(int(900 / 3))

        self.populate_tree()

    def run_hal_cmd(self, *args):
        try:
            output = subprocess.check_output(
                ["halcmd"] + list(args),
                stderr=subprocess.STDOUT,
                text=True,
            )
            return output.strip()
        except FileNotFoundError:
            return "Error: hal command not found. Is LinuxCNC installed?"
        except subprocess.CalledProcessError as exc:
            return exc.output.strip() or str(exc)

    def get_hal_list(self, hal_type):
        raw = self.run_hal_cmd("list", hal_type)
        if raw.startswith("Error:"):
            return [raw]
        return [item for item in raw.split() if item]

    def get_hal_show(self, hal_type, name):
        if hal_type == "comp":
            command = ["show", "comp", name]
        else:
            command = ["show", hal_type, name]
        return self.run_hal_cmd(*command)

    def populate_tree(self):
        self.tree_store.clear()
        for hal_type, label in HAL_GROUPS:
            parent = self.tree_store.append(None, [label, label, hal_type, "group"])
            items = self.get_hal_list(hal_type)
            for item in items:
                self.append_hierarchical_item(parent, item, hal_type)

    def append_hierarchical_item(self, parent, name, hal_type):
        parts = name.split('.')
        current_parent = parent
        full_path = ""

        for index, part in enumerate(parts):
            full_path = part if index == 0 else f"{full_path}.{part}"
            is_leaf = index == len(parts) - 1
            child = self.find_child(current_parent, part)
            if child is None:
                row_type = "item" if is_leaf else "group"
                child = self.tree_store.append(
                    current_parent,
                    [part, full_path, hal_type, row_type],
                )
            current_parent = child

    def find_child(self, parent_iter, name):
        if parent_iter is None:
            return None
        child = self.tree_store.iter_children(parent_iter)
        while child is not None:
            if self.tree_store[child][0] == name:
                return child
            child = self.tree_store.iter_next(child)
        return None

    def on_tree_selection_changed(self, selection):
        model, tree_iter = selection.get_selected()
        if tree_iter is None:
            return

        row_type = model[tree_iter][3]
        if row_type != "item":
            self.detail_buffer.set_text("")
            return

        name = model[tree_iter][1]
        hal_type = model[tree_iter][2]
        details = self.get_hal_show(hal_type, name)
        self.detail_buffer.set_text(details)


def main():
    win = HalShowWindow()
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()

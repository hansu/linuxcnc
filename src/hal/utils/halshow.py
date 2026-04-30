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
from gi.repository import Gtk, GLib


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

        # Menu bar
        menubar = Gtk.MenuBar()
        main_box.pack_start(menubar, False, False, 0)

        file_menu = Gtk.Menu()
        file_item = Gtk.MenuItem(label="File")
        file_item.set_submenu(file_menu)
        menubar.append(file_item)

        exit_item = Gtk.MenuItem(label="Exit")
        exit_item.connect("activate", lambda _w: Gtk.main_quit())
        file_menu.append(exit_item)

        watch_menu = Gtk.Menu()
        watch_item = Gtk.MenuItem(label="Watch")
        watch_item.set_submenu(watch_menu)
        menubar.append(watch_item)

        add_pin_item = Gtk.MenuItem(label="Add Pin")
        add_pin_item.connect("activate", lambda _w: self.add_to_watch("pin"))
        watch_menu.append(add_pin_item)

        add_sig_item = Gtk.MenuItem(label="Add Signal")
        add_sig_item.connect("activate", lambda _w: self.add_to_watch("sig"))
        watch_menu.append(add_sig_item)

        add_param_item = Gtk.MenuItem(label="Add Parameter")
        add_param_item.connect("activate", lambda _w: self.add_to_watch("param"))
        watch_menu.append(add_param_item)

        watch_menu.append(Gtk.SeparatorMenuItem())

        clear_watch_item = Gtk.MenuItem(label="Clear Watch")
        clear_watch_item.connect("activate", lambda _w: self.clear_watch())
        watch_menu.append(clear_watch_item)

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

        # Right side: Notebook with SHOW and WATCH tabs
        self.notebook = Gtk.Notebook()
        self.notebook.set_tab_pos(Gtk.PositionType.TOP)

        # SHOW tab
        show_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        show_label = Gtk.Label(label="Details")
        show_label.set_xalign(0.0)
        show_box.pack_start(show_label, False, False, 0)

        self.detail_textview = Gtk.TextView()
        self.detail_textview.set_editable(False)
        self.detail_textview.set_cursor_visible(False)
        self.detail_buffer = self.detail_textview.get_buffer()
        show_scroll = Gtk.ScrolledWindow()
        show_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        show_scroll.add(self.detail_textview)
        show_box.pack_start(show_scroll, True, True, 0)

        self.notebook.append_page(show_box, Gtk.Label(label="SHOW"))

        # WATCH tab
        watch_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        watch_toolbar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        add_pin_button = Gtk.Button(label="Add Pin")
        add_pin_button.connect("clicked", lambda _btn: self.add_to_watch("pin"))
        watch_toolbar.pack_start(add_pin_button, False, False, 0)
        add_sig_button = Gtk.Button(label="Add Signal")
        add_sig_button.connect("clicked", lambda _btn: self.add_to_watch("sig"))
        watch_toolbar.pack_start(add_sig_button, False, False, 0)
        add_param_button = Gtk.Button(label="Add Parameter")
        add_param_button.connect("clicked", lambda _btn: self.add_to_watch("param"))
        watch_toolbar.pack_start(add_param_button, False, False, 0)
        refresh_watch_button = Gtk.Button(label="Refresh")
        refresh_watch_button.connect("clicked", lambda _btn: self.update_watch_view())
        watch_toolbar.pack_start(refresh_watch_button, False, False, 0)
        clear_button = Gtk.Button(label="Clear Watch")
        clear_button.connect("clicked", lambda _btn: self.clear_watch())
        watch_toolbar.pack_start(clear_button, False, False, 0)
        watch_box.pack_start(watch_toolbar, False, False, 0)

        self.watch_store = Gtk.ListStore(str, str, str)  # Name, Type, Value
        self.watch_view = Gtk.TreeView(model=self.watch_store)
        name_column = Gtk.TreeViewColumn("Name", Gtk.CellRendererText(), text=0)
        self.watch_view.append_column(name_column)
        type_column = Gtk.TreeViewColumn("Type", Gtk.CellRendererText(), text=1)
        self.watch_view.append_column(type_column)
        value_column = Gtk.TreeViewColumn("Value", Gtk.CellRendererText(), text=2)
        self.watch_view.append_column(value_column)

        watch_scroll = Gtk.ScrolledWindow()
        watch_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        watch_scroll.add(self.watch_view)
        watch_box.pack_start(watch_scroll, True, True, 0)

        self.notebook.append_page(watch_box, Gtk.Label(label="WATCH"))

        paned.add1(tree_scroll)
        paned.add2(self.notebook)
        paned.set_position(int(900 / 3))

        self.watch_list = []  # List of (name, type) tuples

        self.populate_tree()

        # Auto-refresh watch tab every 1 second
        GLib.timeout_add(1000, self.update_watch_view)

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

        # Automatically add to watch
        if (name, hal_type) not in self.watch_list:
            self.watch_list.append((name, hal_type))
            self.update_watch_view()

    def add_to_watch(self, hal_type):
        selection = self.tree_view.get_selection()
        model, tree_iter = selection.get_selected()
        if tree_iter is None:
            return
        row_type = model[tree_iter][3]
        if row_type != "item":
            return
        name = model[tree_iter][1]
        selected_type = model[tree_iter][2]
        if selected_type != hal_type:
            return  # Only add if types match
        if (name, hal_type) not in self.watch_list:
            self.watch_list.append((name, hal_type))
            self.update_watch_view()

    def clear_watch(self):
        self.watch_list.clear()
        self.watch_store.clear()

    def update_watch_view(self):
        if not self.watch_list:
            return True  # Keep timer running
        self.watch_store.clear()
        for name, hal_type in self.watch_list:
            value = self.get_hal_value(hal_type, name)
            self.watch_store.append([name, hal_type, value])
        return True  # Keep timer running

    def get_hal_value(self, hal_type, name):
        if hal_type == "pin":
            output = self.run_hal_cmd("getp", name)
        elif hal_type == "param":
            output = self.run_hal_cmd("getp", name)
        elif hal_type == "sig":
            output = self.run_hal_cmd("gets", name)
        else:
            return "N/A"
        return output if output else "N/A"


def main():
    win = HalShowWindow()
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()

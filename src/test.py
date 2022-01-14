#!/usr/bin/env python

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import GObject
from gi.repository import GLib
import hal
from hal_glib import GStat
GSTAT = GStat()


# force GSTAT to initialize states
GSTAT.forced_update()

print("tool:", GSTAT.get_current_tool())
print("geometry:", GSTAT.get_geometry())

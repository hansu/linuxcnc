#!/usr/bin/env python3
import sys, os
import gettext
import linuxcnc, hal
import nf, rs274.options
import tkinter
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
from gi.repository import GLib

BASE = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), ".."))
gettext.install("linuxcnc", localedir=os.path.join(BASE, "share", "locale"))

_after = None
def poll_hal_in_background():
    print("poll hal in background")
    global _after
    _after = None
    if not h.change:
        app.tk.call("destroy", ".tool_change")
        return

    if (h.change_button):
        h.changed = True
        app.update()
        app.tk.call("destroy", ".tool_change")
        stop_polling_hal_in_background()
        return

    _after = app.after(100, poll_hal_in_background)

def start_polling_hal_in_background():
    global _after
    _after = app.after(100, poll_hal_in_background)

def stop_polling_hal_in_background():
    global _after
    if _after: app.after_cancel(_after)
    _after = None

def periodic():
    print("periodic")
    global dialog
    if not h.change:
        try:            
            dialog.response(Gtk.ResponseType.ACCEPT)
            # print(dialog.get_title())
        except:
            print(" +++++++++++++ no dialog")
        return False

    if (h.change_button):
        h.changed = True
        try:
            print(" +++++++++++++ change button 1")
            dialog.response(Gtk.ResponseType.ACCEPT)
            dialog.show()
            print(" +++++++++++++ change button 2")
            # print(dialog.get_title())
        except:
            print(" +++++++++++++ no dialog")
        return False
    return True

def do_change(n):
    print("+++++++ do change")
    if n:
        message = _("Insert tool %d and click continue when ready") % n
    else:
        message = _("Remove the tool and click continue when ready")
    app.wm_withdraw()
    app.update()
    start_polling_hal_in_background()

    global dialog
    try:
        # todo
        if 1:  
            win = Gtk.Window()
            dialog = Gtk.Dialog(_("Tool change"), win, None)
            dialog.connect("delete-event", lambda w, d: w.run())
            dialog.set_default_size(300, -1)            
            label = Gtk.Label.new(message)
            label.set_line_wrap(True)
            button = Gtk.Button.new_with_mnemonic(_("_Continue"))
            button.set_size_request(-1, 60)
            button.connect("clicked",lambda w:dialog.response(Gtk.ResponseType.OK))
            box = Gtk.HButtonBox()
            box.add(button)
            dialog.vbox.pack_start(label, True, True, 0)
            dialog.vbox.pack_end(box, True, True, 0)
            # dialog.set_border_width(0)
            dialog.show_all()
            timer_id = GLib.timeout_add(100, periodic)
            print("++++++ run start")
            r = dialog.run()
            print("++++++ run end")
            print ("-->    r=", r)
            GLib.source_remove(timer_id) # this or return False in cb
            try:
                dialog.destroy()               
                win.destroy() 
                win.show_all()
                
            except:
                print("Error")
        else:
            r = app.tk.call("tk_dialog", ".tool_change",
                _("Tool change"), message, "info", 0, _("Continue"))
    finally:
        stop_polling_hal_in_background()
    if isinstance(r, str): r = int(r)
    # todo:
    if r == 0 or r == Gtk.ResponseType.OK:
        h.changed = True
    app.update()

def withdraw():
    app.wm_withdraw()
    app.bind("<Expose>", lambda event: app.wm_withdraw())

h = hal.component("hal_manualtoolchange")
h.newpin("number", hal.HAL_S32, hal.HAL_IN)
h.newpin("change", hal.HAL_BIT, hal.HAL_IN)
h.newpin("change_button", hal.HAL_BIT, hal.HAL_IN)
h.newpin("changed", hal.HAL_BIT, hal.HAL_OUT)
h.ready()

app = tkinter.Tk(className="AxisToolChanger")
app.wm_geometry("-60-60")
app.wm_title(_("AXIS Manual Toolchanger"))
# rs274.options.install(app)
# nf.start(app); nf.makecommand(app, "_", _)
app.wm_protocol("WM_DELETE_WINDOW", app.wm_withdraw)
lab = tkinter.Message(app, aspect=500, text = _("\
This window is part of the AXIS manual toolchanger.  It is safe to close \
or iconify this window, or it will close automatically after a few seconds."))
lab.pack()
app.after(5 * 1000, withdraw)


try:
    while 1:
        change = h.change
        if change and not h.changed:
            do_change(h.number)
        elif not change:
            h.changed = False
        app.after(100)
        app.update()
except KeyboardInterrupt:
    pass

#!/usr/bin/env python3
import sys, os
import gettext
import linuxcnc, hal
import nf, rs274.options
import tkinter
import argparse

BASE = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), ".."))
gettext.install("linuxcnc", localedir=os.path.join(BASE, "share", "locale"))

parser = argparse.ArgumentParser(description='HAL Manual toolchanger')
parser.add_argument('--gui', type=str, metavar='DISPLAY',  
                    help='User interface corresponding to [DISPLAY] value in inifile (axis, gmoccapy, gscreen ...)',
                    default=None)
parser.add_argument('--force_toolkit', type=str, metavar='TOOLKIT',   
                    help='Force toolkit (Tk/Gtk), ignores gui-setting',
                    default=None)           
opts = parser.parse_args(sys.argv[1:])                                          
                                                
inifile = linuxcnc.ini(os.environ.get('INI_FILE_NAME', '/dev/null'))
display = inifile.find("DISPLAY","DISPLAY")
gtk_strings = ["gmoccapy", "gscreen", "touchy", "qtvcp", "qtvcp woodpecker"]

if opts.force_toolkit in ["Gtk", "gtk", "GTK"]:
    use_gtk = True
elif opts.force_toolkit in ["Tk", "tk", "TK"]:    
    use_gtk = False
else:
    if opts.gui in gtk_strings:
        use_gtk = True
    elif display in gtk_strings:
        use_gtk = True
    else:
        use_gtk = False

if use_gtk:
    import gi
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk
    from gi.repository import GLib

_after = None
def poll_hal_in_background():
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
    global dialog
    if not h.change:
        dialog.response(Gtk.ResponseType.ACCEPT)
        return False

    if (h.change_button):
        h.changed = True
        dialog.response(Gtk.ResponseType.ACCEPT)
        return False
    return True

def do_change(n):
    if n:
        message = _("Insert tool %d and click continue when ready") % n
    else:
        message = _("Remove the tool and click continue when ready")
    app.wm_withdraw()
    app.update()
    start_polling_hal_in_background()

    global dialog
    try:
        if use_gtk:
            win = Gtk.Window()
            dialog = Gtk.Dialog(_("Tool change"), win, None)
            dialog.connect("delete-event", lambda w, d: w.run())
            dialog.set_default_size(300, -1)            
            label = Gtk.Label.new(message)
            label.set_line_wrap(True)
            button = Gtk.Button.new_with_mnemonic(_("_Continue"))
            button.set_size_request(-1, 56)
            button.connect("clicked",lambda w:dialog.response(Gtk.ResponseType.OK))
            box = Gtk.HButtonBox()
            box.add(button)
            dialog.vbox.pack_start(label, True, True, 0)
            dialog.vbox.pack_end(box, True, True, 0)
            dialog.set_border_width(0)
            dialog.show_all()
            timer_id = GLib.timeout_add(100, periodic)
            r = dialog.run()
            if r == Gtk.ResponseType.OK:
                r = 0
            else:
                r = -1
            # GLib.source_remove(timer_id) # this or return False in cb
            dialog.destroy()               
            win.destroy() 
            win.show_all()

        else:
            # with nf_dialog and Gtk import following error: _tkinter.TclError: bad screen distance ".25"
            r = app.tk.call("nf_dialog", ".tool_change",
                _("Tool change"), message, "info", 0, _("Continue"))
    finally:
        stop_polling_hal_in_background()
    if isinstance(r, str): r = int(r)
    if r == 0:
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
rs274.options.install(app)
nf.start(app); nf.makecommand(app, "_", _)
app.wm_protocol("WM_DELETE_WINDOW", app.wm_withdraw)
lab = tkinter.Message(app, aspect=500, text = _("\
This window is part of the AXIS manual toolchanger.  It is safe to close \
or iconify this window, or it will close automatically after a few seconds."))
lab.pack()
app.after(10 * 1000, withdraw)


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

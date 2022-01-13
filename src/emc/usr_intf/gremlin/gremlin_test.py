#!/usr/bin/env python3

#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.



import sys
import glnav
import rs274.glcanon
import linuxcnc
import gcode
import tempfile
import shutil
import os


class DummyProgress:
    def nextphase(self, unused): pass
    def progress(self): pass

class StatCanon(rs274.glcanon.GLCanon, rs274.interpret.StatMixin):
    def __init__(self, colors, geometry, lathe_view_option, stat, random):
        rs274.glcanon.GLCanon.__init__(self, colors, geometry)
        rs274.interpret.StatMixin.__init__(self, stat, random)
        self.progress = DummyProgress()

class GCodeProperties(rs274.glcanon.GlCanonDraw, glnav.GlNavBase):
    def __init__(self, inifile):
        glnav.GlNavBase.__init__(self)
        def C(s):
            a = self.colors[s + "_alpha"]
            s = self.colors[s]
            return [int(x * 255) for x in s + (a,)]
        self.inifile = inifile

        rs274.glcanon.GlCanonDraw.__init__(self, linuxcnc.stat(), None)
        temp = inifile.find("DISPLAY", "LATHE")
        self.lathe_option = bool(temp == "1" or temp == "True" or temp == "true" )
        self.metric_units = True
        self.gcode_properties = None
        self.g0 = 0
        self.g1 = 0
        self.gt = 0
        self.min_extents = [9e99,9e99,9e99]
        self.max_extents = [-9e99,-9e99,-9e99]

    def get_gcode_properties(self): return self.gcode_properties

    def get_geometry(self):
        temp = self.inifile.find("DISPLAY", "GEOMETRY")
        if temp:
            geometry = re.split(" *(-?[XYZABCUVW])", temp.upper())
            self.geometry = "".join(reversed(geometry))
        else:
            self.geometry = 'XYZ'
        return self.geometry


    def load(self, filename = None):
        s = self.stat
        s.poll()
        if not filename and s.file:
            filename = s.file
        elif not filename and not s.file:
            return

        td = tempfile.mkdtemp()
        self._current_file = filename
        try:
            random = int(self.inifile.find("EMCIO", "RANDOM_TOOLCHANGER") or 0)
            canon = StatCanon(self.colors, self.get_geometry(),self.lathe_option, s, random)
            parameter = self.inifile.find("RS274NGC", "PARAMETER_FILE")
            temp_parameter = os.path.join(td, os.path.basename(parameter or "linuxcnc.var"))
            if parameter:
                shutil.copy(parameter, temp_parameter)
            canon.parameter_file = temp_parameter

            unitcode = "G%d" % (20 + (s.linear_units == 1))
            initcode = self.inifile.find("RS274NGC", "RS274NGC_STARTUP_CODE") or ""
            result, seq = self.load_preview(filename, canon, unitcode, initcode)
            if result > gcode.MIN_ERROR:
                self.report_gcode_error(result, seq, filename)
            self.calculate_gcode_properties(canon)

        finally:
            shutil.rmtree(td)


    def from_internal_linear_unit(self, v, unit=None):
        if unit is None:
            unit = self.stat.linear_units
        lu = (unit or 1) * 25.4
        return v*lu    

    def calculate_gcode_properties(self, canon):
        def dist(xxx_todo_changeme, xxx_todo_changeme1):
            (x,y,z) = xxx_todo_changeme
            (p,q,r) = xxx_todo_changeme1
            return ((x-p)**2 + (y-q)**2 + (z-r)**2) ** .5
        def from_internal_units(pos, unit=None):
            if unit is None:
                unit = self.stat.linear_units
            lu = (unit or 1) * 25.4

            lus = [lu, lu, lu, 1, 1, 1, lu, lu, lu]
            return [a*b for a, b in zip(pos, lus)]

        props = {}
        loaded_file = self._current_file
        max_speed = float(
            self.inifile.find("DISPLAY","MAX_LINEAR_VELOCITY")
            or self.inifile.find("TRAJ","MAX_LINEAR_VELOCITY")
            or self.inifile.find("AXIS_X","MAX_VELOCITY")
            or 1)

        if not loaded_file:
            props['name'] = "No file loaded"
        else:
            ext = os.path.splitext(loaded_file)[1]
            program_filter = None
            if ext:
                program_filter = self.inifile.find("FILTER", ext[1:])
            name = os.path.basename(loaded_file)
            if program_filter:
                props['name'] = "generated from %s" % name
            else:
                props['name'] = name

            size = os.stat(loaded_file).st_size
            lines = sum(1 for line in open(loaded_file))
            props['size'] = "%(size)s bytes\n%(lines)s gcode lines" % {'size': size, 'lines': lines}

            if self.metric_units:
                conv = 1
                units = "mm"
                fmt = "%.3f"
            else:
                conv = 1/25.4
                units = "in"
                fmt = "%.4f"

            mf = max_speed

            self.g0 = sum(dist(l[1][:3], l[2][:3]) for l in canon.traverse)
            self.g1 = (sum(dist(l[1][:3], l[2][:3]) for l in canon.feed) +
                sum(dist(l[1][:3], l[2][:3]) for l in canon.arcfeed))
            self.gt = (sum(dist(l[1][:3], l[2][:3])/min(mf, l[3]) for l in canon.feed) +
                sum(dist(l[1][:3], l[2][:3])/min(mf, l[3])  for l in canon.arcfeed) +
                sum(dist(l[1][:3], l[2][:3])/mf  for l in canon.traverse) +
                canon.dwell_time
                )
 
            props['G0'] = "%f %s".replace("%f", fmt) % (self.from_internal_linear_unit(self.g0, conv), units)
            props['G1'] = "%f %s".replace("%f", fmt) % (self.from_internal_linear_unit(self.g1, conv), units)
            if self.gt > 120:
                props['Run'] = "%.1f Minutes" % (self.gt/60)
            else:
                props['Run'] = "%d Seconds" % (int(self.gt))

            self.min_extents = from_internal_units(canon.min_extents, conv)
            self.max_extents = from_internal_units(canon.max_extents, conv)
            # print("min extends:", min_extents)
            # print("max extends:", max_extents)
            for (i, c) in enumerate("XYZ"):
                a = self.min_extents[i]
                b = self.max_extents[i]
                if a != b:
                    props[c] = "%(a)f to %(b)f = %(diff)f %(units)s".replace("%f", fmt) % {'a': a, 'b': b, 'diff': b-a, 'units': units}
            props['Units'] = units

            if self.metric_units:
                if 200 in canon.state.gcodes:
                    gcode_units = "in"
                else:
                    gcode_units = "mm"
            else:
                if 210 in canon.state.gcodes:
                    gcode_units = "mm"
                else:
                    gcode_units = "in"
            props['GCode Units'] = gcode_units

        self.gcode_properties = props


if __name__ == '__main__':

    # s = linuxcnc.stat()
    # s.poll()
    # canon = StatCanon(rs274.glcanon.GlCanonDraw.colors, "XYZ", False, s, 0)
    # load("/home/cnc/linuxcnc/nc_files/examples/3dtest.ngc")
    # print(canon.traverse)


    inifile = linuxcnc.ini("/home/cnc/linuxcnc/configs/Sieg-X1-dev/Sieg-X1.ini")
    # inifile = linuxcnc.ini(os.getenv("INI_FILE_NAME"))
    # inifile = linuxcnc.ini(os.environ['INI_FILE_NAME'],None)
  
    t = GCodeProperties(inifile)
    t.load() #"/home/cnc/linuxcnc/nc_files/examples/3dtest.ngc")
    props = t.get_gcode_properties()
    print("size:", props.get('size'))
    print("G0:", props.get('G0'))
    print("G1:", props.get('G1'))
    print("Time:", props.get('Run'))
    print("min:", t.min_extents)
    print("max:", t.max_extents)
    print("zeit:", t.gt/60)


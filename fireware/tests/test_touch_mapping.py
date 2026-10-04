"""Compile and exercise the actual GT911 landscape mapping on the host (MSVC)."""
import argparse
import ctypes as c
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIB = None


def mapped(x, y, rx=320, ry=480, swap=False, mx=False, my=False):
    ox, oy = c.c_uint16(0xFFFF), c.c_uint16(0xFFFF)
    ok = LIB.map_point(x, y, rx, ry, swap, mx, my, c.byref(ox), c.byref(oy))
    return bool(ok), ox.value, oy.value


class TouchMappingTests(unittest.TestCase):
    def test_native_corners(self):
        # Vendor landscape 2: sensor top-left -> display bottom-left.
        for raw, screen in [((0, 0), (0, 319)), ((319, 0), (0, 0)),
                            ((0, 479), (479, 319)), ((319, 479), (479, 0))]:
            self.assertEqual(mapped(*raw), (True, *screen))

    def test_full_native_grid_is_bijective_and_bounded(self):
        points = {mapped(x, y)[1:] for x in range(320) for y in range(480)}
        self.assertEqual(points, {(x, y) for x in range(480) for y in range(320)})

    def test_resolution_and_calibration(self):
        for rx, ry in [(800, 480), (65535, 65535), (2, 2)]:
            self.assertEqual(mapped(0, 0, rx, ry), (True, 0, 319))
            self.assertEqual(mapped(rx - 1, ry - 1, rx, ry), (True, 479, 0))
        # A swapped 480x320 sensor should normalize to the same portrait space.
        self.assertEqual(mapped(479, 0, 480, 320, swap=True), (True, 479, 319))
        self.assertEqual(mapped(0, 0, mx=True), (True, 0, 0))
        self.assertEqual(mapped(0, 0, my=True), (True, 479, 319))
        self.assertEqual(mapped(0, 0, mx=True, my=True), (True, 479, 0))

    def test_invalid_does_not_modify_output(self):
        for args in [(320, 0), (0, 480), (0, 0, 0, 480), (0, 0, 320, 1)]:
            self.assertEqual(mapped(*args), (False, 0xFFFF, 0xFFFF))
        out = c.c_uint16()
        self.assertFalse(LIB.map_point(0, 0, 320, 480, False, False, False, None, c.byref(out)))
        self.assertFalse(LIB.map_point(0, 0, 320, 480, False, False, False, c.byref(out), None))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--msvc-bin', type=Path, default=Path(
        'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64'))
    parser.add_argument('--sdk-include', type=Path, default=Path(
        'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0'))
    args = parser.parse_args()
    build = ROOT / 'tests/build'
    build.mkdir(parents=True, exist_ok=True)
    source, obj, dll = [build / ('touch_mapping.' + ext) for ext in ('c', 'obj', 'dll')]
    source.write_text('''#include "touch_mapping.h"
bool map_point(uint16_t x, uint16_t y, uint16_t rx, uint16_t ry,
               bool swap, bool mx, bool my, uint16_t *ox, uint16_t *oy)
{
    return touch_map_landscape(x, y, rx, ry, swap, mx, my, ox, oy);
}
''', encoding='utf-8')
    subprocess.run([str(args.msvc_bin / 'cl.exe'), '/nologo', '/TC', '/std:c11',
        '/GS-', '/Zl', '/W4', '/WX', '/O2',
        '/I' + str(args.msvc_bin.parents[2] / 'include'),
        '/I' + str(args.sdk_include / 'ucrt'), '/I' + str(args.sdk_include / 'shared'),
        '/I' + str(ROOT / 'components/board_drivers/include'), '/c', str(source),
        '/Fo' + str(obj)], check=True)
    subprocess.run([str(args.msvc_bin / 'link.exe'), '/nologo', '/dll', '/noentry',
        '/nodefaultlib', '/machine:x64', '/export:map_point', f'/out:{dll}', str(obj)], check=True)
    LIB = c.CDLL(str(dll))
    LIB.map_point.argtypes = [c.c_uint16] * 4 + [c.c_bool] * 3 + [c.POINTER(c.c_uint16)] * 2
    LIB.map_point.restype = c.c_bool
    unittest.main(argv=['test_touch_mapping'], verbosity=2)

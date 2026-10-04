"""Compile optional example/orientation branches with the built IDF commands.

Run after build.ps1 build. Writes only tests/build/optional; does not change the
default sdkconfig or firmware image. This is a compile check, not a device test.
"""
import ctypes as c
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
commands = json.loads((root / 'build/compile_commands.json').read_text(encoding='utf-8'))
output = root / 'tests/build/optional'
output.mkdir(parents=True, exist_ok=True)
parse = c.windll.shell32.CommandLineToArgvW
parse.argtypes = [c.c_wchar_p, c.POINTER(c.c_int)]
parse.restype = c.POINTER(c.c_wchar_p)
free = c.windll.kernel32.LocalFree
free.argtypes = [c.c_void_p]
free.restype = c.c_void_p
variants = [
    ('app_main.c', 'all_examples_even', [
        'BOARD_LCD_COLOR_TEST', 'BOARD_SERIAL_ECHO', 'BOARD_MODBUS_DEMO',
        'BOARD_MODBUS1_SLAVE=1', 'BOARD_MODBUS1_START_REGISTER=0',
        'BOARD_MODBUS2_SLAVE=2', 'BOARD_MODBUS2_START_REGISTER=10',
        'BOARD_RS4852_PARITY_ODD', 'BOARD_RS4852_TWO_STOP_BITS',
        'BOARD_RS4851_PARITY_EVEN', 'BOARD_RS4851_TWO_STOP_BITS']),
    ('app_main.c', 'odd_parity', ['BOARD_RS4851_PARITY_ODD', 'BOARD_RS4852_PARITY_EVEN']),
    ('touch_gt911.c', 'touch_orientation', [
        'BOARD_TOUCH_SWAP_XY', 'BOARD_TOUCH_MIRROR_X', 'BOARD_TOUCH_MIRROR_Y']),
    ('app_main.c', 'no_gui_color_bars', ['!BOARD_LVGL_BENCHMARK', 'BOARD_LCD_COLOR_TEST']),
    ('board_lvgl.c', 'no_gui_port', ['!BOARD_LVGL_BENCHMARK']),
]
for source, name, macros in variants:
    entry = next(item for item in commands if Path(item['file']).name == source)
    count = c.c_int()
    ptr = parse(entry['command'], c.byref(count))
    if not ptr:
        raise OSError('Cannot parse compiler command')
    try:
        args = [ptr[i] for i in range(count.value)]
    finally:
        free(c.cast(ptr, c.c_void_p))
    args[args.index('-o') + 1] = str(output / (name + '.obj'))
    if '-MF' in args:
        args[args.index('-MF') + 1] = str(output / (name + '.d'))
    # Include the real configuration first, then override it for this translation
    # unit. sdkconfig.h uses pragma once so source includes cannot undo overrides.
    header = output / (name + '.h')
    lines = ['#include "sdkconfig.h"']
    for macro in macros:
        if macro.startswith('!'):
            lines.append('#undef CONFIG_' + macro[1:])
        else:
            key, _, value = macro.partition('=')
            lines.extend(['#undef CONFIG_' + key, '#define CONFIG_' + key + ' ' + (value or '1')])
    header.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    args += ['-include', str(header)]
    subprocess.run(args, cwd=entry['directory'], check=True)
    print('PASS:', name)

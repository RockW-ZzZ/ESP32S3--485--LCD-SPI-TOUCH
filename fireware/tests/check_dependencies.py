"""Verify bundled LVGL files and optionally prepare an isolated clean build.

The isolated copy has only firmware inputs, no hardware/reference project and
no existing build/sdkconfig cache. Build it with its own build.ps1 afterward.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import uuid

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--prepare-standalone', action='store_true')
args = parser.parse_args()
vendor = ROOT / 'components/lvgl'
manifest = json.loads((vendor / 'SOURCE_SHA256.json').read_text(encoding='utf-8'))
for name, expected in manifest['files'].items():
    path = vendor / name
    assert path.is_file() and not path.is_symlink(), name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, name
for folder in ('components', 'main', 'tools'):
    for path in (ROOT / folder).rglob('*'):
        assert not path.is_symlink(), path
        if path.is_file() and (path.suffix in ('.c', '.h', '.cmake', '.py', '.yml') or path.name == 'CMakeLists.txt'):
            assert 'ref-code' not in path.read_text(encoding='utf-8').lower(), path
print(f"PASS: {len(manifest['files'])} bundled LVGL files; no reference-folder build links")
if args.prepare_standalone:
    dest = ROOT / 'tests/build' / ('standalone_' + uuid.uuid4().hex[:8])
    dest.mkdir(parents=True)
    for folder in ('components', 'main', 'tools'):
        shutil.copytree(ROOT / folder, dest / folder)
    for name in ('CMakeLists.txt', 'build.ps1', 'sdkconfig.defaults'):
        shutil.copy2(ROOT / name, dest / name)
    # Also compile/link the optional two-task demo with different port parameters.
    with (dest / 'sdkconfig.defaults').open('a', encoding='utf-8') as stream:
        stream.write('\nCONFIG_BOARD_MODBUS_DEMO=y\nCONFIG_BOARD_RS4852_BAUD=19200\n'
                     'CONFIG_BOARD_MODBUS2_SLAVE=2\nCONFIG_BOARD_MODBUS2_START_REGISTER=10\n')
    print('Standalone project:', dest)

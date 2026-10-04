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
    data = path.read_bytes()
    candidates = [data]
    if b'\0' not in data:
        # Original bundled LVGL text used CRLF; Git historically stored LF.
        # Permit line-ending conversion only; binary assets remain byte exact.
        lf = data.replace(b'\r\n', b'\n')
        candidates.extend((lf, lf.replace(b'\n', b'\r\n')))
    assert any(hashlib.sha256(value).hexdigest() == expected for value in candidates), name
for folder in ('components', 'main', 'tools'):
    for path in (ROOT / folder).rglob('*'):
        assert not path.is_symlink(), path
        if path.is_file() and (path.suffix in ('.c', '.h', '.cmake', '.py', '.yml') or path.name == 'CMakeLists.txt'):
            assert 'ref-code' not in path.read_text(encoding='utf-8').lower(), path
print(f"PASS: {len(manifest['files'])} bundled LVGL files; no reference-folder build links")
mdns = ROOT / 'components/mdns'
manifest = json.loads((mdns / 'SOURCE_SHA256.json').read_text(encoding='utf-8'))
for name, expected in manifest['files'].items():
    assert hashlib.sha256((mdns / name).read_bytes()).hexdigest() == expected, name
assert (ROOT / 'components/charger/web/index.html').is_file()
assert (ROOT / 'partitions.csv').is_file()
print(f"PASS: {len(manifest['files'])} bundled mDNS files, web UI and OTA partitions")
if args.prepare_standalone:
    dest = ROOT / 'tests/build' / ('standalone_' + uuid.uuid4().hex[:8])
    dest.mkdir(parents=True)
    for folder in ('components', 'main', 'tools'):
        shutil.copytree(ROOT / folder, dest / folder)
    for name in ('CMakeLists.txt', 'build.ps1', 'sdkconfig.defaults', 'partitions.csv'):
        shutil.copy2(ROOT / name, dest / name)
    # Clean build includes charger, Wi-Fi, web asset and OTA partition inputs.
    with (dest / 'sdkconfig.defaults').open('a', encoding='utf-8') as stream:
        stream.write('\nCONFIG_BOARD_CHARGER_APP=y\n')
    print('Standalone project:', dest)

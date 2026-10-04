"""Compile real charger service/codec/policy with deterministic IDF/UART stubs."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
msvc = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231')
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = '10.0.26100.0'
build = root / 'tests/build/charger'
build.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
env['INCLUDE'] = ';'.join(map(str, [msvc / 'include', sdk / 'Include' / version / 'ucrt', sdk / 'Include' / version / 'shared']))
env['LIB'] = ';'.join(map(str, [msvc / 'lib/x64', sdk / 'Lib' / version / 'ucrt/x64', sdk / 'Lib' / version / 'um/x64']))
subprocess.run([str(msvc / 'bin/Hostx64/x64/cl.exe'), '/nologo', '/TC', '/std:c11', '/W4', '/WX', '/wd4244', '/wd4267', '/wd4100', '/MT', '/O2',
    '/I' + str(root / 'tests/charger_stubs'), '/I' + str(root / 'tests/host_stubs'),
    '/I' + str(root / 'components/charger/include'), '/I' + str(root / 'components/board_drivers/include'),
    str(root / 'tests/charger_host.c'), str(root / 'components/charger/charger_codec.c'),
    str(root / 'components/charger/charger_policy.c'), str(root / 'components/board_drivers/modbus_codec.c'),
    '/Fe:charger_test.exe'], cwd=build, env=env, check=True)
subprocess.run([str(build / 'charger_test.exe')], check=True, cwd=build)

"""Run the installed idf.py while retaining this project's NTFS ASCII alias.

IDF 6.1 calls realpath on -C/-B, expanding 8.3 names back to Chinese. Its
toolchain response-file generator then corrupts non-ASCII path characters.
Limit the workaround to paths INSIDE this project and to this Python process.
No installed ESP-IDF files, global settings, source copies or drive maps change.
"""
import ctypes
import os
from pathlib import Path
import runpy
import sys

original_realpath = os.path.realpath
project = original_realpath(Path(__file__).parent.parent)
short_buffer = ctypes.create_unicode_buffer(32768)
get_short_path = ctypes.windll.kernel32.GetShortPathNameW
get_short_path.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
get_short_path.restype = ctypes.c_uint32
if not get_short_path(project, short_buffer, len(short_buffer)):
    raise OSError('Cannot obtain NTFS short path for project')
short_project = short_buffer.value
if not short_project.isascii():
    raise OSError('This toolchain requires an ASCII project path or an NTFS 8.3 alias')


def project_realpath(path, *args, **kwargs):
    result = original_realpath(path, *args, **kwargs)
    decoded = os.fsdecode(result)
    normalized = os.path.normcase(decoded)
    root = os.path.normcase(project)
    if normalized == root or normalized.startswith(root + os.sep):
        decoded = short_project + decoded[len(project):]
        return os.fsencode(decoded) if isinstance(result, bytes) else decoded
    return result


os.path.realpath = project_realpath
idf_script = os.path.join(os.environ['IDF_PATH'], 'tools', 'idf.py')
sys.path.insert(0, os.path.dirname(idf_script))
sys.argv = [idf_script] + sys.argv[1:]
runpy.run_path(idf_script, run_name='__main__')
